// Copyright 2010 Google Inc. All Rights Reserved.
// Author: jacobsa@google.com (Aaron Jacobs)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "gjstest/internal/cpp/run_tests.h"

#include <string>
#include <vector>

#include <re2/re2.h>
#include <v8.h>

#include "base/basictypes.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/stl_decl.h"
#include "base/stringprintf.h"
#include "base/timer.h"
#include "gjstest/internal/cpp/test_case.h"
#include "gjstest/internal/cpp/v8_utils.h"
#include "gjstest/internal/proto/named_scripts.pb.h"
#include "strings/strutil.h"
#include "util/gtl/map-util.h"
#include "util/hash/hash.h"
#include "webutil/xml/xml_writer.h"

using v8::Array;
using v8::Arguments;
using v8::Context;
using v8::Function;
using v8::Handle;
using v8::HandleScope;
using v8::Local;
using v8::Object;
using v8::ObjectTemplate;
using v8::Persistent;
using v8::String;
using v8::TryCatch;
using v8::Undefined;
using v8::Value;

namespace gjstest {

// Create XML output given an overall duration in seconds, a list of test names
// in the order of execution, a map from test names to durations, and a map from
// failed test names to failure messages.
static string MakeXml(
    uint32 duration_ms,
    const vector<string>& tests_run,
    const hash_map<string, double>& test_durations,
    const hash_map<string, string>& test_failure_messages) {
  webutil_xml::XmlWriter xml_writer("UTF-8", true);
  xml_writer.StartDocument("UTF-8");

  xml_writer.StartElement("testsuite");
  xml_writer.AddAttribute("name", "Google JS tests");
  xml_writer.AddAttribute("failures", SimpleItoa(test_failure_messages.size()));
  xml_writer.AddAttribute("time", SimpleDtoa(duration_ms / 1000.0));

  for (uint32 i = 0; i < tests_run.size(); ++i) {
    const string& name = tests_run[i];

    // Skip the tests that weren't actually run.
    if (!ContainsKey(test_durations, name)) continue;

    const double duration = FindOrDie(test_durations, name);

    xml_writer.StartElement("testcase");
    xml_writer.AddAttribute("name", name);
    xml_writer.AddAttribute("time", SimpleDtoa(duration));

    // Add a failure element if the test failed.
    if (ContainsKey(test_failure_messages, name)) {
      const string& failure_message = FindOrDie(test_failure_messages, name);

      xml_writer.StartElement("failure");
      xml_writer.WriteCData(failure_message);
      xml_writer.EndElement();  // failure
    }

    xml_writer.EndElement();  // testcase
  }

  xml_writer.EndElement();  // testsuite


  return xml_writer.GetContent();
}

// Get a reference to the function of the supplied name.
static Local<Function> GetFunctionNamed(const string& name) {
  const Local<Value> result = ExecuteJs(name, "");
  CHECK(!result.IsEmpty());
  CHECK(result->IsFunction());

  return Local<Function>::Cast(result);
}

static void ProcessTestCase(
    const string& name,
    const Handle<Function>& test_function,
    bool* success,
    string* output,
    hash_map<string, string>* test_failure_messages,
    hash_map<string, double>* test_durations) {
  // Run the test.
  TestCase test_case(test_function);
  test_case.Run();

  // Append the appropriate stuff to our output.
  StringAppendF(output, "[ RUN      ] %s\n", name.c_str());

  string status_message = "[       OK ]";
  if (!test_case.succeeded) {
    *success = false;
    status_message = "[  FAILED  ]";

    // Record the failure output for use in the XML later. Strip any
    // surrounding whitespace first.
    StripWhiteSpace(&test_case.failure_output);
    InsertOrDie(test_failure_messages, name, test_case.failure_output);
  }

  // Append the test output and the status message.
  StringAppendF(
      output,
      "%s%s %s (%u ms)\n",
      test_case.output.c_str(),
      status_message.c_str(),
      name.c_str(),
      test_case.duration_ms);

  // Record test duration.
  InsertOrDie(test_durations, name, test_case.duration_ms / 1000.0);
}

// Iterate over a map from test names to test functions, running each test
// function.
static void ProcessTestSuite(
    const RE2& test_filter,
    const Handle<Object>& test_functions,
    bool* success,
    string* output,
    vector<string>* tests_run,
    hash_map<string, string>* test_failure_messages,
    hash_map<string, double>* test_durations) {
  StringAppendF(output, "[----------]\n");

  const Local<Array> test_names = test_functions->GetPropertyNames();
  for (uint32 i = 0; i < test_names->Length(); ++i) {
    const Local<Value> name = test_names->Get(i);
    const Local<Value> test_function = test_functions->Get(name);
    CHECK(test_function->IsFunction());

    // Skip this test if it doesn't match our filter.
    const string string_name = ConvertToString(name);
    if (!RE2::FullMatch(string_name, test_filter)) continue;

    tests_run->push_back(string_name);
    ProcessTestCase(
        string_name,
        Local<Function>::Cast(test_function),
        success,
        output,
        test_failure_messages,
        test_durations);
  }

  StringAppendF(output, "[----------]\n\n");
}

bool RunTests(
    const NamedScripts& scripts,
    const string& test_filter_string,
    string* output,
    string* xml) {
  const RE2 test_filter(test_filter_string.empty() ? ".*" : test_filter_string);

  // Take ownership of all handles created.
  HandleScope handle_owner;

  // Create a context in which to run scripts and ensure that it's used whenever
  // a context is needed below.
  Persistent<Context> context = Context::New();
  Context::Scope context_scope(context);

  // Run all of the scripts.
  for (uint32 i = 0; i < scripts.script_size(); ++i) {
    const NamedScript& script = scripts.script(i);

    TryCatch try_catch;
    const Local<Value> result = ExecuteJs(script.source(), script.name());
    if (result.IsEmpty()) {
      *output += DescribeError(try_catch) + "\n";
      return false;
    }
  }

  // Get a reference to gjstest.internal.getTestFunctions for later.
  const Local<Function> get_test_functions =
      GetFunctionNamed("gjstest.internal.getTestFunctions");

  // Keep maps from test name to failure message (if the test failed) and
  // duration in seconds.
  hash_map<string, string> test_failure_messages;
  hash_map<string, double> test_durations;
  vector<string> tests_run;

  // Keep track of how long the whole process takes, and whether there are any
  // failures.
  CycleTimer overall_timer;
  overall_timer.Start();
  bool success = true;

  // Iterate over all of the registered test suites.
  const Local<Value> test_suites_value =
      ExecuteJs("gjstest.internal.testSuites", "");
  CHECK(test_suites_value->IsArray());
  const Local<Array> test_suites = Local<Array>::Cast(test_suites_value);

  for (uint32 i = 0; i < test_suites->Length(); ++i) {
    const Local<Value> test_suite = test_suites->Get(i);
    CHECK(test_suite->IsObject());

    // Get the map of test functions registered for this test suite.
    Handle<Value> args[] = { test_suite };
    const Local<Value> test_functions_value =
        get_test_functions->Call(
            context->Global(),
            arraysize(args),
            args);
    CHECK(test_functions_value->IsObject());
    const Local<Object> test_functions =
        Local<Object>::Cast(test_functions_value);

    // Process this test suite.
    ProcessTestSuite(
        test_filter,
        test_functions,
        &success,
        output,
        &tests_run,
        &test_failure_messages,
        &test_durations);
  }

  overall_timer.Stop();

  StringAppendF(
      output,
      success ? "[  PASSED  ]\n" : "[  FAILED  ]\n");

  // Make sure that at least one test ran. This catches common errors with
  // mis-registering tests and so on.
  if (test_durations.empty()) {
    *output = "No tests found.\n";
    return false;
  }

  // Create an XML document describing the execution.
  *xml =
      MakeXml(
          overall_timer.GetInMs(),
          tests_run,
          test_durations,
          test_failure_messages);

  return success;
}

}  // namespace gjstest
