/**
 *
 * Copyright 2018 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <google/protobuf/compiler/code_generator.h>
#include <google/protobuf/compiler/plugin.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/printer.h>
#include <google/protobuf/io/zero_copy_stream.h>

using google::protobuf::Descriptor;
using google::protobuf::FileDescriptor;
using google::protobuf::MethodDescriptor;
using google::protobuf::ServiceDescriptor;
using google::protobuf::compiler::CodeGenerator;
using google::protobuf::compiler::GeneratorContext;
using google::protobuf::compiler::ParseGeneratorParameter;
using google::protobuf::compiler::PluginMain;
using google::protobuf::io::Printer;
using google::protobuf::io::ZeroCopyOutputStream;

namespace grpc {
namespace web {
namespace {

using std::string;

enum Mode {
  OP = 0,          // first party google3 one platform services
  GATEWAY = 1,     // open-source gRPC Gateway
  OPJSPB = 2,      // first party google3 one platform services with JSPB
  FRAMEWORKS = 3,  // first party google3 AF services with AF data add-ons
  GRPCWEB = 4,     // client using the application/grpc-web wire format
};

enum ImportStyle {
  CLOSURE = 0,     // goog.require("grpc.web.*")
  COMMONJS = 1,    // const grpcWeb = require("grpc-web")
};

string GetModeVar(const Mode mode) {
  switch (mode) {
    case OP:
      return "OP";
    case GATEWAY:
      return "Gateway";
    case OPJSPB:
      return "OPJspb";
    case FRAMEWORKS:
      return "Frameworks";
    case GRPCWEB:
      return "GrpcWeb";
  }
  return "";
}

string GetDeserializeMethodName(const string& mode_var) {
  if (mode_var == GetModeVar(Mode::OPJSPB)) {
    return "deserialize";
  }
  return "deserializeBinary";
}

string GetSerializeMethodName(const string& mode_var) {
  if (mode_var == GetModeVar(Mode::OPJSPB)) {
    return "serialize";
  }
  return "serializeBinary";
}

string LowercaseFirstLetter(string s) {
  if (s.empty()) {
    return s;
  }
  s[0] = ::tolower(s[0]);
  return s;
}


// The following 5 functions were copied from
// google/protobuf/src/google/protobuf/stubs/strutil.h

inline bool HasPrefixString(const string& str,
                            const string& prefix) {
  return str.size() >= prefix.size() &&
      str.compare(0, prefix.size(), prefix) == 0;
}

inline string StripPrefixString(const string& str, const string& prefix) {
  if (HasPrefixString(str, prefix)) {
    return str.substr(prefix.size());
  } else {
    return str;
  }
}

inline bool HasSuffixString(const string& str,
                            const string& suffix) {
  return str.size() >= suffix.size() &&
      str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline string StripSuffixString(const string& str, const string& suffix) {
  if (HasSuffixString(str, suffix)) {
    return str.substr(0, str.size() - suffix.size());
  } else {
    return str;
  }
}

void ReplaceCharacters(string *s, const char *remove, char replacewith) {
  const char *str_start = s->c_str();
  const char *str = str_start;
  for (str = strpbrk(str, remove);
       str != nullptr;
       str = strpbrk(str + 1, remove)) {
    (*s)[str - str_start] = replacewith;
  }
}


// The following function was copied from
// google/protobuf/src/google/protobuf/compiler/cpp/cpp_helpers.cc

string StripProto(const string& filename) {
  if (HasSuffixString(filename, ".protodevel")) {
    return StripSuffixString(filename, ".protodevel");
  } else {
    return StripSuffixString(filename, ".proto");
  }
}


// The following 3 functions were copied from
// google/protobuf/src/google/protobuf/compiler/js/js_generator.cc

// Returns the name of the message with a leading dot and taking into account
// nesting, for example ".OuterMessage.InnerMessage", or returns empty if
// descriptor is null. This function does not handle namespacing, only message
// nesting.
string GetNestedMessageName(const Descriptor* descriptor) {
  if (descriptor == nullptr) {
    return "";
  }
  string result = StripPrefixString(descriptor->full_name(),
                                    descriptor->file()->package());
  // Add a leading dot if one is not already present.
  if (!result.empty() && result[0] != '.') {
    result = "." + result;
  }
  return result;
}

// Given a filename like foo/bar/baz.proto, returns the root directory
// path ../../
string GetRootPath(const string& from_filename, const string& to_filename) {
  if (HasPrefixString(to_filename, "google/protobuf")) {
    // Well-known types (.proto files in the google/protobuf directory) are
    // assumed to come from the 'google-protobuf' npm package.  We may want to
    // generalize this exception later by letting others put generated code in
    // their own npm packages.
    return "google-protobuf/";
  }

  size_t slashes = std::count(from_filename.begin(), from_filename.end(), '/');
  if (slashes == 0) {
    return "./";
  }
  string result = "";
  for (size_t i = 0; i < slashes; i++) {
    result += "../";
  }
  return result;
}

// Returns the alias we assign to the module of the given .proto filename
// when importing.
string ModuleAlias(const string& filename) {
  // This scheme could technically cause problems if a file includes any 2 of:
  //   foo/bar_baz.proto
  //   foo_bar_baz.proto
  //   foo_bar/baz.proto
  //
  // We'll worry about this problem if/when we actually see it.  This name isn't
  // exposed to users so we can change it later if we need to.
  string basename = StripProto(filename);
  ReplaceCharacters(&basename, "-", '$');
  ReplaceCharacters(&basename, "/", '_');
  ReplaceCharacters(&basename, ".", '_');
  return basename + "_pb";
}

/* Finds all message types used in all services in the file, and returns them
 * as a map of fully qualified message type name to message descriptor */
std::map<string, const Descriptor*> GetAllMessages(const FileDescriptor* file) {
  std::map<string, const Descriptor*> message_types;
  for (int service_index = 0;
       service_index < file->service_count();
       ++service_index) {
    const ServiceDescriptor* service = file->service(service_index);
    for (int method_index = 0;
         method_index < service->method_count();
         ++method_index) {
      const MethodDescriptor *method = service->method(method_index);
      const Descriptor *input_type = method->input_type();
      const Descriptor *output_type = method->output_type();
      message_types[input_type->full_name()] = input_type;
      message_types[output_type->full_name()] = output_type;
    }
  }

  return message_types;
}

void PrintMessagesDeps(Printer* printer, const FileDescriptor* file) {
  std::map<string, const Descriptor*> messages = GetAllMessages(file);
  std::map<string, string> vars;
  for (std::map<string, const Descriptor*>::iterator it = messages.begin();
       it != messages.end(); it++) {
    vars["full_name"] = it->first;
    printer->Print(
        vars,
        "goog.require('proto.$full_name$');\n");
  }
  printer->Print("\n\n\n");
}

void PrintCommonJsMessagesDeps(Printer* printer, const FileDescriptor* file) {
  std::map<string, string> vars;

  for (int i = 0; i < file->dependency_count(); i++) {
    const string& name = file->dependency(i)->name();
    vars["alias"] = ModuleAlias(name);
    vars["dep_filename"] = GetRootPath(file->name(), name) + StripProto(name);
    // we need to give each cross-file import an alias
    printer->Print(
        vars,
        "\nvar $alias$ = require('$dep_filename$_pb.js')\n");
  }

  string package = file->package();
  vars["package_name"] = package;

  printer->Print(vars, "const proto = {};\n");
  if (!package.empty()) {
    size_t offset = 0;
    size_t dotIndex = package.find('.');

    while (dotIndex != string::npos) {
      vars["current_package_ns"] = package.substr(0, dotIndex);
      printer->Print(vars, "proto.$current_package_ns$ = {};\n");

      offset = dotIndex + 1;
      dotIndex = package.find(".", offset);
    }
  }

  // need to import the messages from our own file
  string filename = StripProto(file->name());
  size_t last_slash = filename.find_last_of('/');
  if (last_slash != string::npos) {
    filename = filename.substr(last_slash + 1);
  }
  vars["filename"] = filename;

  printer->Print(
      vars,
      "proto.$package_name$ = require('./$filename$_pb.js');\n\n");
}

void PrintFileHeader(Printer* printer, const std::map<string, string>& vars) {
  printer->Print(
      vars,
      "/**\n"
      " * @fileoverview gRPC Web JS generated client stub for $package$\n"
      " * @enhanceable\n"
      " * @public\n"
      " */\n"
      "// GENERATED CODE -- DO NOT EDIT!\n\n\n");
}

void PrintServiceConstructor(Printer* printer,
                             std::map<string, string> vars) {
  printer->Print(
      vars,
      "/**\n"
      " * @param {string} hostname\n"
      " * @param {?Object} credentials\n"
      " * @param {?Object} options\n"
      " * @constructor\n"
      " * @struct\n"
      " * @final\n"
      " */\n"
      "proto.$package_dot$$service_name$Client =\n"
      "    function(hostname, credentials, options) {\n"
      "  if (!options) options = {};\n");
  if (vars["mode"] == GetModeVar(Mode::GRPCWEB)) {
    printer->Print(
        vars,
        "  options['format'] = '$format$';\n\n");
  }
  printer->Print(
      vars,
      "  /**\n"
      "   * @private @const {!grpc.web.$mode$ClientBase} The client\n"
      "   */\n"
      "  this.client_ = new grpc.web.$mode$ClientBase(options);\n\n"
      "  /**\n"
      "   * @private @const {string} The hostname\n"
      "   */\n"
      "  this.hostname_ = hostname;\n\n"
      "  /**\n"
      "   * @private @const {?Object} The credentials to be used to connect\n"
      "   *    to the server\n"
      "   */\n"
      "  this.credentials_ = credentials;\n\n"
      "  /**\n"
      "   * @private @const {?Object} Options for the client\n"
      "   */\n"
      "  this.options_ = options;\n"
      "};\n\n\n");
}

void PrintMethodInfo(Printer* printer, std::map<string, string> vars) {
  printer->Print(
      vars,
      "/**\n"
      " * @const\n"
      " * @type {!grpc.web.AbstractClientBase.MethodInfo<\n"
      " *   !proto.$in$,\n"
      " *   !proto.$out$>}\n"
      " */\n"
      "const methodInfo_$method_name$ = "
      "new grpc.web.AbstractClientBase.MethodInfo(\n");
  printer->Indent();
  printer->Print(
      vars,
      "$out_type$,\n"
      "/** @param {!proto.$in$} request */\n"
      "function(request) {\n");
  printer->Print(
      ("  return request." + GetSerializeMethodName(vars["mode"]) +
       "();\n").c_str());
  printer->Print("},\n");
  printer->Print(
      vars,
      ("$out_type$." + GetDeserializeMethodName(vars["mode"]) +
       "\n").c_str());
  printer->Outdent();
  printer->Print(
      vars,
      ");\n\n\n");
}

void PrintUnaryCall(Printer* printer, std::map<string, string> vars) {
  PrintMethodInfo(printer, vars);
  printer->Print(
      vars,
      "/**\n"
      " * @param {!proto.$in$} request The\n"
      " *     request proto\n"
      " * @param {!Object<string, string>} metadata User defined\n"
      " *     call metadata\n"
      " * @param {function(?grpc.web.Error,"
      " ?proto.$out$)}\n"
      " *     callback The callback function(error, response)\n"
      " * @return {!grpc.web.ClientReadableStream<!proto.$out$>|undefined}\n"
      " *     The XHR Node Readable Stream\n"
      " */\n"
      "proto.$package_dot$$service_name$Client.prototype.$js_method_name$ =\n");
  printer->Indent();
  printer->Print(vars,
                 "  function(request, metadata, callback) {\n"
                 "return this.client_.rpcCall(this.hostname_ +\n");
  printer->Indent();
  printer->Indent();
  if (vars["mode"] == GetModeVar(Mode::OP) ||
      vars["mode"] == GetModeVar(Mode::OPJSPB)) {
    printer->Print(vars,
                   "'/$$rpc/$package_dot$$service_name$/$method_name$',\n");
  } else {
    printer->Print(vars, "'/$package_dot$$service_name$/$method_name$',\n");
  }
  printer->Print(
      vars,
      "request,\n"
      "metadata,\n"
      "methodInfo_$method_name$,\n"
      "callback);\n");
  printer->Outdent();
  printer->Outdent();
  printer->Outdent();
  printer->Print("};\n\n\n");
}

void PrintServerStreamingCall(Printer* printer, std::map<string, string> vars) {
  PrintMethodInfo(printer, vars);
  printer->Print(
      vars,
      "/**\n"
      " * @param {!proto.$in$} request The request proto\n"
      " * @param {!Object<string, string>} metadata User defined\n"
      " *     call metadata\n"
      " * @return {!grpc.web.ClientReadableStream<!proto.$out$>}\n"
      " *     The XHR Node Readable Stream\n"
      " */\n"
      "proto.$package_dot$$service_name$Client.prototype.$js_method_name$ =\n");
  printer->Indent();
  printer->Print(
      "  function(request, metadata) {\n"
      "return this.client_.serverStreaming(this.hostname_ +\n");
  printer->Indent();
  printer->Indent();
  if (vars["mode"] == GetModeVar(Mode::OP) ||
      vars["mode"] == GetModeVar(Mode::OPJSPB)) {
    printer->Print(vars,
                   "'/$$rpc/$package_dot$$service_name$/$method_name$',\n");
  } else {
    printer->Print(vars, "'/$package_dot$$service_name$/$method_name$',\n");
  }
  printer->Print(
      vars,
      "request,\n"
      "metadata,\n"
      "methodInfo_$method_name$);\n");
  printer->Outdent();
  printer->Outdent();
  printer->Outdent();
  printer->Print("};\n\n\n");
}

class GrpcCodeGenerator : public CodeGenerator {
 public:
  GrpcCodeGenerator() {}
  ~GrpcCodeGenerator() override {}

  bool Generate(const FileDescriptor* file, const string& parameter,
                GeneratorContext* context, string* error) const override {
    if (!file->service_count()) {
      // No services, nothing to do.
      return true;
    }

    std::vector<std::pair<string, string> > options;
    ParseGeneratorParameter(parameter, &options);

    string file_name;
    string mode;
    string import_style_str;
    ImportStyle import_style;

    for (size_t i = 0; i < options.size(); ++i) {
      if (options[i].first == "out") {
        file_name = options[i].second;
      } else if (options[i].first == "mode") {
        mode = options[i].second;
      } else if (options[i].first == "import_style") {
        import_style_str = options[i].second;
      } else {
        *error = "unsupported options: " + options[i].first;
        return false;
      }
    }

    if (file_name.empty()) {
      file_name = StripProto(file->name()) + "_grpc_pb.js";
    }
    if (mode.empty()) {
      *error = "options: mode is required";
      return false;
    }

    std::map<string, string> vars;
    string package = file->package();
    vars["package"] = package;
    vars["package_dot"] = package.empty() ? "" : package + '.';

    if (mode == "binary") {
      vars["mode"] = GetModeVar(Mode::OP);
    } else if (mode == "base64") {
      vars["mode"] = GetModeVar(Mode::GATEWAY);
    } else if (mode == "grpcweb" || mode == "grpcwebtext") {
      vars["mode"] = GetModeVar(Mode::GRPCWEB);
      vars["format"] = (mode == "grpcweb") ? "binary" : "text";
    } else if (mode == "jspb") {
      vars["mode"] = GetModeVar(Mode::OPJSPB);
    } else if (mode == "frameworks") {
      vars["mode"] = GetModeVar(Mode::FRAMEWORKS);
    } else {
      *error = "options: invalid mode - " + mode;
      return false;
    }

    if (import_style_str == "closure" || import_style_str.empty()) {
      import_style = ImportStyle::CLOSURE;
    } else if (import_style_str == "commonjs") {
      import_style = ImportStyle::COMMONJS;
    } else {
      *error = "options: invalid import_style - " + import_style_str;
      return false;
    }

    std::unique_ptr<ZeroCopyOutputStream> output(
        context->Open(file_name));
    Printer printer(output.get(), '$');
    PrintFileHeader(&printer, vars);

    for (int i = 0; i < file->service_count(); ++i) {
      const ServiceDescriptor* service = file->service(i);
      vars["service_name"] = service->name();
      switch (import_style) {
        case ImportStyle::CLOSURE:
          printer.Print(
              vars,
              "goog.provide('proto.$package_dot$$service_name$Client');\n");
          break;
        case ImportStyle::COMMONJS:
          break;
      }
    }
    printer.Print("\n");

    switch (import_style) {
      case ImportStyle::CLOSURE:
        printer.Print(vars, "goog.require('grpc.web.$mode$ClientBase');\n");
        printer.Print(vars, "goog.require('grpc.web.AbstractClientBase');\n");
        printer.Print(vars, "goog.require('grpc.web.ClientReadableStream');\n");
        printer.Print(vars, "goog.require('grpc.web.Error');\n");

        PrintMessagesDeps(&printer, file);
        printer.Print("goog.scope(function() {\n\n");
        break;
      case ImportStyle::COMMONJS:
        printer.Print(vars, "const grpc = {};\n");
        printer.Print(vars, "grpc.web = require('grpc-web');\n\n");
        PrintCommonJsMessagesDeps(&printer, file);
        break;
    }

    for (int service_index = 0;
         service_index < file->service_count();
         ++service_index) {
      const ServiceDescriptor* service = file->service(service_index);
      vars["service_name"] = service->name();
      PrintServiceConstructor(&printer, vars);

      for (int method_index = 0;
           method_index < service->method_count();
           ++method_index) {
        const MethodDescriptor* method = service->method(method_index);
        vars["js_method_name"] = LowercaseFirstLetter(method->name());
        vars["method_name"] = method->name();
        vars["in"] = method->input_type()->full_name();
        vars["out"] = method->output_type()->full_name();
        if (import_style == ImportStyle::COMMONJS &&
            method->output_type()->file() != file) {
          // Cross-file ref in CommonJS needs to use the module alias instead
          // of the global name.
          vars["out_type"] = ModuleAlias(method->output_type()->file()->name())
                             + GetNestedMessageName(method->output_type());
        } else {
          vars["out_type"] = "proto."+method->output_type()->full_name();
        }

        // Client streaming is not supported yet
        if (!method->client_streaming()) {
          if (method->server_streaming()) {
            PrintServerStreamingCall(&printer, vars);
          } else {
            PrintUnaryCall(&printer, vars);
          }
        }
      }
    }

    switch (import_style) {
      case ImportStyle::CLOSURE:
        printer.Print("}); // goog.scope\n\n");
        break;
      case ImportStyle::COMMONJS:
        printer.Print(vars, "module.exports = proto.$package$;\n\n");
        break;
    }

    return true;
  }
};

}  // namespace
}  // namespace web
}  // namespace grpc

int main(int argc, char* argv[]) {
  grpc::web::GrpcCodeGenerator generator;
  PluginMain(argc, argv, &generator);
  return 0;
}
