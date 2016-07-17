// Copyright 2015 Las Venturas Playground. All rights reserved.
// Use of this source code is governed by the MIT license, a copy of which can
// be found in the LICENSE file.

#include "bindings/global_scope.h"

#include <fstream>
#include <sstream>

#include "base/logging.h"
#include "base/time.h"
#include "bindings/event.h"
#include "bindings/console.h"
#include "bindings/exception_handler.h"
#include "bindings/global_callbacks.h"
#include "bindings/modules/streamer_module.h"
#include "bindings/modules/mysql_module.h"
#include "bindings/pawn_invoke.h"
#include "bindings/promise.h"
#include "bindings/runtime.h"
#include "bindings/runtime_operations.h"
#include "bindings/timer_queue.h"
#include "bindings/utilities.h"
#include "performance/scoped_trace.h"

namespace bindings {

GlobalScope::GlobalScope(plugin::PluginController* plugin_controller)
    : finalized_(false),
      console_(new Console),
      pawn_invoke_(new PawnInvoke(plugin_controller)),
      streamer_module_(new StreamerModule),
      mysql_module_(new MySQLModule) {}

GlobalScope::~GlobalScope() {}

void GlobalScope::RegisterEvent(const std::string& type, std::unique_ptr<Event> event) {
  events_[type].swap(event);
}

void GlobalScope::InstallPrototypes(v8::Local<v8::ObjectTemplate> global) {
  // Install the event listener functions (as defined by HTML's EventTarget interface, although
  // we add support for hasEventListeners since it matters for internal performance).
  InstallFunction(global, "addEventListener", AddEventListenerCallback);
  InstallFunction(global, "dispatchEvent", DispatchEventCallback);
  InstallFunction(global, "hasEventListener", HasEventListenersCallback);
  InstallFunction(global, "removeEventListener", RemoveEventListenerCallback);

  // Install the other functions that should be available on |global|.
  InstallFunction(global, "frameCounter", FrameCounterCallback);
  InstallFunction(global, "highResolutionTime", HighResolutionTimeCallback);
  InstallFunction(global, "pawnInvoke", PawnInvokeCallback);
  InstallFunction(global, "provideNative", ProvideNativeCallback);
  InstallFunction(global, "requireImpl", RequireImplCallback);
  InstallFunction(global, "startTrace", StartTraceCallback);
  InstallFunction(global, "stopTrace", StopTraceCallback);
  InstallFunction(global, "wait", WaitCallback);

  // Used for telling the test runner (if it's enabled) that the JavaScript tests have finished.
  InstallFunction(global, "reportTestsFinished", ReportTestsFinishedCallback);
  InstallFunction(global, "notifyReady", NotifyReadyCallback);

  // TODO(Russell): Provide some kind of filesystem module.
  InstallFunction(global, "glob", GlobCallback);
  InstallFunction(global, "readFile", ReadFileCallback);

  // Install the Console and MySQL interfaces.
  console_->InstallPrototype(global);

  mysql_module_->InstallPrototypes(global);
  streamer_module_->InstallPrototypes(global);

  // Install the interfaces associated with each of the dynamically created events.
  for (const auto& pair : events_)
    pair.second->InstallPrototype(global);
}

void GlobalScope::InstallObjects(v8::Local<v8::Object> global) {
  // Install the "self" object, which refers to the global scope (for compatibility with
  // Web Workers and Document in Web development, which also expose "self").
  global->Set(v8String("self"), global);

  // Install the global instance of the Console object.
  console_->InstallObjects(global);
}

void GlobalScope::Finalize() {
  finalized_ = true;
}

Event* GlobalScope::GetEvent(const std::string& type) {
  const auto event_iter = events_.find(type);
  if (event_iter == events_.end())
    return nullptr;

  return event_iter->second.get();
}

void GlobalScope::AddEventListener(const std::string& type, v8::Local<v8::Function> listener) {
  event_listeners_[type].push_back(
      v8::Persistent<v8::Function>(v8::Isolate::GetCurrent(), listener));
}

bool GlobalScope::DispatchEvent(const std::string& type, v8::Local<v8::Value> event) const {
  auto event_list_iter = event_listeners_.find(type);
  if (event_list_iter == event_listeners_.end())
    return false;  // this can happen for developer-defined callbacks.

  v8::Isolate* isolate = v8::Isolate::GetCurrent();

  // Initialize an array with the |event| value that will be available.
  v8::Local<v8::Value> arguments[1];
  arguments[0] = event;

  ScopedExceptionSource source("dispatched event `" + type + "`");

  for (const auto& persistent_function : event_list_iter->second) {
    if (persistent_function.IsEmpty()) {
      LOG(WARNING) << "[v8] Empty function found for event " << type;
      continue;
    }

    // Convert the persistent function to a local one again, without losing the persistent reference
    // (which may be done if the listener removes itself from the event target).
    v8::Local<v8::Function> function = v8::Local<v8::Function>::New(isolate, persistent_function);

    if (function.IsEmpty()) {
      LOG(WARNING) << "[v8] Unable to coerce the persistent funtion to a local for event " << type;
      continue;

    } else {
      //performance::ScopedTrace trace(performance::INTERCEPTED_CALLBACK_EVENT_HANDLER,
      //                               type, function->GetScriptOrigin(), function->GetScriptLineNumber());

      Call(isolate, function, arguments, 1u);
    }
  }

  return Event::DefaultPrevented(event);
}

bool GlobalScope::HasEventListeners(const std::string& type) const {
  auto event_list_iter = event_listeners_.find(type);
  if (event_list_iter == event_listeners_.end())
    return false;

  return event_list_iter->second.size() > 0;
}

double GlobalScope::HighResolutionTime() const {
  return base::monotonicallyIncreasingTime();
}

void GlobalScope::RemoveEventListener(const std::string& type, v8::Local<v8::Function> listener) {
  auto event_list_iter = event_listeners_.find(type);
  if (event_list_iter == event_listeners_.end())
    return;

  // Remove all associated event listeners if the |listener| was not passed.
  if (listener.IsEmpty()) {
    event_listeners_.erase(event_list_iter);
    return;
  }

  // Attempt to find the |listener| in the list of listeners associated with event |type|. If it's
  // found, remove it, and continue - it's possible to register listeners multiple times.
  auto event_listener_iter = event_list_iter->second.begin();
  while (event_listener_iter != event_list_iter->second.end()) {
    if (listener == *event_listener_iter)
      event_listener_iter = event_list_iter->second.erase(event_listener_iter);
    else
      event_listener_iter++;
  }
}

std::string GlobalScope::ReadFile(const std::string& filename) const {
  std::ifstream handle(filename.c_str());
  if (!handle.is_open() || handle.fail()) {
    ThrowException("unable to execute readFile(): file " + filename + " does not exist.");
    return std::string();
  }
    
  std::stringstream content_stream;
  std::copy(std::istreambuf_iterator<char>(handle),
            std::istreambuf_iterator<char>(),
            std::ostreambuf_iterator<char>(content_stream));

  return content_stream.str();
}

v8::Local<v8::Value> GlobalScope::RequireImpl(Runtime* runtime, const std::string& filename) const {
  performance::ScopedTrace trace(performance::LOAD_JAVASCRIPT_TRACE, filename);

  v8::Local<v8::Value> result;
  if (!runtime->ExecuteFile(base::FilePath(filename), Runtime::EXECUTION_TYPE_MODULE, &result))
    ThrowException("unable to execute require(): cannot evaluate " + filename + ".");

  return result;
}

v8::Local<v8::Promise> GlobalScope::Wait(Runtime* runtime, int64_t time) {
  std::shared_ptr<Promise> promise(new Promise);

  runtime->GetTimerQueue()->Add(promise, time);

  return promise->GetPromise();
}

void GlobalScope::InstallFunction(v8::Local<v8::ObjectTemplate> global,
                                  const std::string& name, v8::FunctionCallback callback) {
  global->Set(v8String(name),
              v8::FunctionTemplate::New(v8::Isolate::GetCurrent(), callback));
}

}  // namespace bindings
