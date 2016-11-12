// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/modular/src/story_runner/module_controller_impl.h"

#include "apps/modular/src/story_runner/story_impl.h"
#include "lib/fidl/cpp/bindings/interface_handle.h"
#include "lib/fidl/cpp/bindings/interface_ptr.h"
#include "lib/fidl/cpp/bindings/interface_request.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/tasks/task_runner.h"
#include "lib/ftl/time/time_delta.h"
#include "lib/mtl/tasks/message_loop.h"

namespace modular {

namespace {

constexpr ftl::TimeDelta kStoryTearDownTimeout = ftl::TimeDelta::FromSeconds(1);

}  // namespace

ModuleControllerImpl::ModuleControllerImpl(
    StoryImpl* const story_impl,
    const fidl::String& url,
    fidl::InterfacePtr<Module> module,
    fidl::InterfaceRequest<ModuleController> module_controller)
    : story_impl_(story_impl),
      url_(url),
      module_(std::move(module)),
      binding_(this, std::move(module_controller)) {
  FTL_LOG(INFO) << "ModuleControllerImpl " << url_;

  // If the Module instance closes its own connection, we tear it
  // down.
  module_.set_connection_error_handler([this]() { TearDown([]() {}); });
}

ModuleControllerImpl::~ModuleControllerImpl() {
  FTL_LOG(INFO) << "~ModuleControllerImpl " << url_;
}

void ModuleControllerImpl::Done() {
  FTL_LOG(INFO) << "ModuleControllerImpl::Done() " << url_;
  for (auto& watcher : watchers_) {
    watcher->OnDone();
  }
};

void ModuleControllerImpl::TearDown(std::function<void()> done) {
  teardown_.push_back(done);

  FTL_LOG(INFO) << "ModuleControllerImpl::TearDown() " << url_ << " "
                << teardown_.size();

  if (teardown_.size() != 1) {
    // Not the first request, Stop() in progress.
    return;
  }

  // Copyable such that all copies of the function share the same
  // 'called' variable. This function causes this to be deleted when
  // called once, but may be called twice, so the second call must be
  // protected from fully executing.
  //
  // We also capture url so it can be used for logging in the second
  // invocation.
  auto cont = ftl::MakeCopyable([ this, called = false, url = url_ ]() mutable {
    FTL_LOG(INFO) << "ModuleControllerImpl::TearDown() CONT " << url << " "
                  << called;

    if (called) {
      return;
    }
    called = true;

    module_.reset();

    for (auto& watcher : watchers_) {
      watcher->OnStop();
    }

    // Value of teardown must survive deletion of this.
    auto teardown = teardown_;

    story_impl_->Dispose(this);

    for (auto& done : teardown) {
      done();
    }

    FTL_LOG(INFO) << "ModuleControllerImpl::TearDown() DONE " << url;
  });

  // Call Module.Stop(), but also schedule a timeout.
  module_->Stop(cont);

  mtl::MessageLoop::GetCurrent()->task_runner()->PostDelayedTask(
      cont, kStoryTearDownTimeout);
}

void ModuleControllerImpl::Watch(fidl::InterfaceHandle<ModuleWatcher> watcher) {
  watchers_.push_back(
      fidl::InterfacePtr<ModuleWatcher>::Create(std::move(watcher)));
}

void ModuleControllerImpl::Stop(const StopCallback& done) {
  FTL_LOG(INFO) << "ModuleControllerImpl::Stop() " << url_;
  TearDown(done);
}

}  // namespace modular