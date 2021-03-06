// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/modular/lib/fidl/single_service_app.h"
#include "apps/modular/lib/testing/reporting.h"
#include "apps/modular/lib/testing/testing.h"
#include "apps/modular/services/component/component_context.fidl.h"
#include "apps/modular/services/story/module.fidl.h"
#include "lib/mtl/tasks/message_loop.h"

using modular::testing::TestPoint;

namespace {

// This is how long we wait for the test to finish before we timeout and tear
// down our test.
constexpr int kTimeoutMilliseconds = 5000;
constexpr char kTest1Agent[] =
    "file:///system/apps/modular_tests/component_context_test_agent1";

class ParentApp : public modular::SingleServiceApp<modular::Module> {
 public:
  ParentApp() { modular::testing::Init(application_context(), __FILE__); }

  ~ParentApp() override { mtl::MessageLoop::GetCurrent()->PostQuitTask(); }

 private:
  // |Module|
  void Initialize(
      fidl::InterfaceHandle<modular::Story> story,
      fidl::InterfaceHandle<modular::Link> link,
      fidl::InterfaceHandle<app::ServiceProvider> incoming_services,
      fidl::InterfaceRequest<app::ServiceProvider> outgoing_services) override {
    story_.Bind(std::move(story));
    link_.Bind(std::move(link));
    initialized_.Pass();

    // Exercise ComponentContext.ConnectToAgent()
    modular::ComponentContextPtr ctx;
    story_->GetComponentContext(ctx.NewRequest());

    app::ServiceProviderPtr agent_services;
    ctx->ConnectToAgent(kTest1Agent, agent_services.NewRequest(),
                        agent_controller_.NewRequest());

    modular::testing::GetStore()->Get(
        "test_agent1_connected", [this](const fidl::String&) {
          agent_connected_.Pass();

          // Closing the agent controller should trigger the agent to stop.
          agent_controller_.reset();

          modular::testing::GetStore()->Get("test_agent1_stopped",
                                            [this](const fidl::String&) {
                                              agent_stopped_.Pass();
                                              story_->Done();
                                            });
        });

    // Start a timer to call Story.Done in case the test agent misbehaves and we
    // time out.
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

  modular::StoryPtr story_;
  modular::LinkPtr link_;
  modular::AgentControllerPtr agent_controller_;

  TestPoint initialized_{"Root module initialized"};
  TestPoint stopped_{"Root module stopped"};
  TestPoint agent_connected_{"Agent1 accepted connection"};
  TestPoint agent_stopped_{"Agent1 stopped"};
};

}  // namespace

int main(int argc, const char** argv) {
  mtl::MessageLoop loop;
  new ParentApp();
  loop.Run();
  TEST_PASS("Root module exited");
  modular::testing::Teardown();
  return 0;
}
