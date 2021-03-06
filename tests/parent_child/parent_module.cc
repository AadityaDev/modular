// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <iostream>

#include "apps/modular/lib/fidl/single_service_app.h"
#include "apps/modular/lib/testing/reporting.h"
#include "apps/modular/lib/testing/testing.h"
#include "apps/modular/services/story/module.fidl.h"
#include "apps/mozart/services/views/view_token.fidl.h"
#include "lib/mtl/tasks/message_loop.h"

using modular::testing::TestPoint;

namespace {

constexpr int kTimeoutMilliseconds = 5000;

constexpr char kChildModule[] = "file:///system/apps/modular_tests/child_module";

class ParentApp : public modular::SingleServiceApp<modular::Module> {
 public:
  ParentApp() {
    modular::testing::Init(application_context(), __FILE__);
  }

  ~ParentApp() override { mtl::MessageLoop::GetCurrent()->PostQuitTask(); }

 private:
  // |Module|
  void Initialize(
      fidl::InterfaceHandle<modular::Story> story,
      fidl::InterfaceHandle<modular::Link> link,
      fidl::InterfaceHandle<app::ServiceProvider> incoming_services,
      fidl::InterfaceRequest<app::ServiceProvider> outgoing_services) override {
    initialized_.Pass();
    story_.Bind(std::move(story));
    link_.Bind(std::move(link));

    StartModule(kChildModule);

    modular::testing::GetStore()->Get("child_module_init",
                                      [this](const fidl::String&) {
                                        module_->Stop([this] {
                                          callback_.Pass();
                                          story_->Done();
                                        });
                                      });

    mtl::MessageLoop::GetCurrent()->task_runner()->PostDelayedTask(
        [this] { delete this; },
        ftl::TimeDelta::FromMilliseconds(kTimeoutMilliseconds));
  }

  // |Module|
  void Stop(const StopCallback& done) override {
    stopped_.Pass();
    done();
    delete this;
  }

  void StartModule(const std::string& module_query) {
    modular::LinkPtr child_link;
    story_->CreateLink("child", child_link.NewRequest());
    fidl::InterfaceHandle<mozart::ViewOwner> module_view;
    story_->StartModule(module_query, std::move(child_link), nullptr, nullptr,
                        module_.NewRequest(), module_view.NewRequest());
  }

  modular::StoryPtr story_;
  modular::LinkPtr link_;
  modular::ModuleControllerPtr module_;

  TestPoint initialized_{"Parent module initialized"};
  TestPoint callback_{"Stop child callback invoked"};
  TestPoint stopped_{"Parent module stopped"};
};

}  // namespace

int main(int argc, const char** argv) {
  mtl::MessageLoop loop;
  new ParentApp();
  loop.Run();
  TEST_PASS("Parent module exited");
  modular::testing::Teardown();
  return 0;
}
