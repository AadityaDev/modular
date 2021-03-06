// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A Module that serves as the recipe in the example story, i.e. that
// creates other Modules in the story.

#include "application/lib/app/connect.h"
#include "apps/ledger/services/public/ledger.fidl.h"
#include "apps/modular/examples/counter_cpp/calculator.fidl.h"
#include "apps/modular/examples/counter_cpp/store.h"
#include "apps/modular/lib/fidl/array_to_string.h"
#include "apps/modular/lib/fidl/single_service_view_app.h"
#include "apps/modular/services/story/module.fidl.h"
#include "apps/modular/services/story/story.fidl.h"
#include "apps/mozart/lib/skia/skia_vmo_surface.h"
#include "apps/mozart/lib/view_framework/base_view.h"
#include "apps/mozart/services/geometry/cpp/geometry_util.h"
#include "apps/mozart/services/views/view_manager.fidl.h"
#include "lib/fidl/cpp/bindings/binding_set.h"
#include "lib/fidl/cpp/bindings/interface_request.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/macros.h"
#include "lib/mtl/tasks/message_loop.h"

#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkRect.h"

namespace {

using modular::to_array;
using modular::to_string;
using modular::examples::Adder;

constexpr uint32_t kViewResourceIdBase = 100;
constexpr uint32_t kViewResourceIdSpacing = 100;

constexpr uint32_t kRootNodeId = mozart::kSceneRootNodeId;
constexpr uint32_t kViewNodeIdBase = 100;
constexpr uint32_t kViewNodeIdSpacing = 100;
constexpr uint32_t kViewSceneNodeIdOffset = 1;

// JSON data
constexpr char kInitialJson[] =
    "{     \"@type\" : \"http://schema.domokit.org/PingPongPacket\","
    "      \"http://schema.domokit.org/counter\" : 0,"
    "      \"http://schema.org/sender\" : \"RecipeImpl\""
    "}";

constexpr char kJsonSchema[] =
    "{"
    "  \"$schema\": \"http://json-schema.org/draft-04/schema#\","
    "  \"type\": \"object\","
    "  \"properties\": {"
    "    \"counters\": {"
    "      \"type\": \"object\","
    "      \"properties\": {"
    "        \"http://google.com/id/dc7cade7-7be0-4e23-924d-df67e15adae5\": {"
    "          \"type\": \"object\","
    "          \"properties\": {"
    "            \"@type\": {"
    "              \"type\": \"string\""
    "            },"
    "            \"http://schema.domokit.org/counter\": {"
    "              \"type\": \"integer\""
    "            },"
    "            \"http://schema.org/sender\": {"
    "              \"type\": \"string\""
    "            }"
    "          },"
    "          \"additionalProperties\" : false,"
    "          \"required\": ["
    "            \"@type\","
    "            \"http://schema.domokit.org/counter\","
    "            \"http://schema.org/sender\""
    "          ]"
    "        }"
    "      },"
    "      \"additionalProperties\" : false,"
    "      \"required\": ["
    "        \"http://google.com/id/dc7cade7-7be0-4e23-924d-df67e15adae5\""
    "      ]"
    "    }"
    "  },"
    "  \"additionalProperties\" : false,"
    "  \"required\": ["
    "    \"counters\""
    "  ]"
    "}";

// Ledger keys
constexpr char kLedgerCounterKey[] = "counter_key";

using modular::operator<<;

// Implementation of the LinkWatcher service that forwards each document
// changed in one Link instance to a second Link instance.
class LinkConnection : public modular::LinkWatcher {
 public:
  LinkConnection(modular::Link* const src, modular::Link* const dst)
      : src_binding_(this), src_(src), dst_(dst) {
    src_->Watch(src_binding_.NewBinding());
  }

  void Notify(const fidl::String& json) override {
    // We receive an initial update when the Link initializes. It's empty
    // if this is a new session, or it has documents if it's a restored session.
    // In either case, it should be ignored, otherwise we can get multiple
    // messages traveling at the same time.
    if (!initial_update_ && json.size() > 0) {
      dst_->Set(nullptr, json);
    }
    initial_update_ = false;
  }

 private:
  fidl::Binding<modular::LinkWatcher> src_binding_;
  modular::Link* const src_;
  modular::Link* const dst_;
  bool initial_update_ = true;

  FTL_DISALLOW_COPY_AND_ASSIGN(LinkConnection);
};

class ModuleMonitor : public modular::ModuleWatcher {
 public:
  ModuleMonitor(modular::ModuleController* const module_client,
                modular::Story* const story)
      : binding_(this), story_(story) {
    module_client->Watch(binding_.NewBinding());
  }

  void OnStateChange(modular::ModuleState new_state) override {
    if (new_state == modular::ModuleState::DONE) {
      FTL_LOG(INFO) << "RecipeImpl DONE";
      story_->Done();
    }
  }

 private:
  fidl::Binding<modular::ModuleWatcher> binding_;
  modular::Story* const story_;
  FTL_DISALLOW_COPY_AND_ASSIGN(ModuleMonitor);
};

struct ViewData {
  explicit ViewData(uint32_t key) : key(key) {}
  const uint32_t key;
  mozart::ViewInfoPtr view_info;
  mozart::ViewPropertiesPtr view_properties;
  mozart::RectF layout_bounds;
  uint32_t scene_version = 1u;
};

class RecipeView : public mozart::BaseView {
 public:
  explicit RecipeView(
      mozart::ViewManagerPtr view_manager,
      fidl::InterfaceRequest<mozart::ViewOwner> view_owner_request)
      : BaseView(std::move(view_manager),
                 std::move(view_owner_request),
                 "RecipeView") {}

  ~RecipeView() override = default;

  void ConnectView(fidl::InterfaceHandle<mozart::ViewOwner> view_owner) {
    const uint32_t key = next_child_key_++;
    GetViewContainer()->AddChild(key, std::move(view_owner));
    views_.emplace(
        std::make_pair(key, std::unique_ptr<ViewData>(new ViewData(key))));
  }

 private:
  // |BaseView| implementation copied from
  // https://github.com/fuchsia-mirror/mozart/blob/master/examples/tile/tile_view.cc
  // |BaseView|:
  void OnChildAttached(uint32_t child_key,
                       mozart::ViewInfoPtr child_view_info) override {
    auto it = views_.find(child_key);
    FTL_DCHECK(it != views_.end()) << "Invalid child_key.";
    auto view_data = it->second.get();
    view_data->view_info = std::move(child_view_info);
    Invalidate();
  }

  // |BaseView|:
  void OnChildUnavailable(uint32_t child_key) override {
    auto it = views_.find(child_key);
    FTL_DCHECK(it != views_.end()) << "Invalid child_key.";
    FTL_LOG(ERROR) << "View died unexpectedly: child_key=" << child_key;
    std::unique_ptr<ViewData> view_data = std::move(it->second);
    views_.erase(it);
    GetViewContainer()->RemoveChild(child_key, nullptr);
    Invalidate();
  }

  // |BaseView|:
  void OnLayout() override {
    FTL_DCHECK(properties());
    // Layout all children in a row.
    if (!views_.empty()) {
      const mozart::Size& size = *properties()->view_layout->size;

      uint32_t index = 0;
      uint32_t space = size.width;
      uint32_t base = space / views_.size();
      uint32_t excess = space % views_.size();
      uint32_t offset = 0;
      for (auto it = views_.begin(); it != views_.end(); ++it, ++index) {
        auto view_data = it->second.get();

        // Distribute any excess width among the leading children.
        uint32_t extent = base;
        if (excess) {
          extent++;
          excess--;
        }

        view_data->layout_bounds.x = offset;
        view_data->layout_bounds.y = 0;
        view_data->layout_bounds.width = extent;
        view_data->layout_bounds.height = size.height;
        offset += extent;

        auto view_properties = mozart::ViewProperties::New();
        view_properties->view_layout = mozart::ViewLayout::New();
        view_properties->view_layout->size = mozart::Size::New();
        view_properties->view_layout->size->width =
            view_data->layout_bounds.width;
        view_properties->view_layout->size->height =
            view_data->layout_bounds.height;

        if (view_data->view_properties.Equals(view_properties))
          continue;  // no layout work to do

        view_data->view_properties = view_properties.Clone();
        view_data->scene_version++;
        GetViewContainer()->SetChildProperties(
            it->first, view_data->scene_version, std::move(view_properties));
      }
    }
  }

  // |BaseView|:
  void OnDraw() override {
    FTL_DCHECK(properties());

    // Update the scene.
    // TODO: only send the resources once, be more incremental
    auto update = mozart::SceneUpdate::New();
    update->clear_resources = true;
    update->clear_nodes = true;

    // Create the root node.
    auto root_node = mozart::Node::New();

    // Add the children.
    for (auto it = views_.cbegin(); it != views_.cend(); it++) {
      const ViewData& view_data = *(it->second.get());
      const uint32_t scene_resource_id =
          kViewResourceIdBase + view_data.key * kViewResourceIdSpacing;
      const uint32_t container_node_id =
          kViewNodeIdBase + view_data.key * kViewNodeIdSpacing;

      mozart::RectF extent;
      extent.width = view_data.layout_bounds.width;
      extent.height = view_data.layout_bounds.height;

      // Create a container to represent the place where the child view
      // will be presented.  The children of the container provide
      // fallback behavior in case the view is not available.
      auto container_node = mozart::Node::New();
      container_node->content_clip = extent.Clone();
      container_node->content_transform = mozart::Transform::New();
      mozart::SetTranslationTransform(container_node->content_transform.get(),
                                      view_data.layout_bounds.x,
                                      view_data.layout_bounds.y, 0.f);

      // If we have the view, add it to the scene.
      if (view_data.view_info) {
        auto scene_resource = mozart::Resource::New();
        scene_resource->set_scene(mozart::SceneResource::New());
        scene_resource->get_scene()->scene_token =
            view_data.view_info->scene_token.Clone();
        update->resources.insert(scene_resource_id, std::move(scene_resource));

        const uint32_t scene_node_id =
            container_node_id + kViewSceneNodeIdOffset;
        auto scene_node = mozart::Node::New();
        scene_node->op = mozart::NodeOp::New();
        scene_node->op->set_scene(mozart::SceneNodeOp::New());
        scene_node->op->get_scene()->scene_resource_id = scene_resource_id;
        update->nodes.insert(scene_node_id, std::move(scene_node));
        container_node->child_node_ids.push_back(scene_node_id);
      }

      // Add the container.
      update->nodes.insert(container_node_id, std::move(container_node));
      root_node->child_node_ids.push_back(container_node_id);
    }

    // Add the root node.
    update->nodes.insert(kRootNodeId, std::move(root_node));
    scene()->Update(std::move(update));

    // Publish the scene.
    scene()->Publish(CreateSceneMetadata());
  }

  std::map<uint32_t, std::unique_ptr<ViewData>> views_;
  uint32_t next_child_key_{};

  FTL_DISALLOW_COPY_AND_ASSIGN(RecipeView);
};

class AdderImpl : public modular::examples::Adder {
 public:
  AdderImpl() {}

 private:
  // |Adder| impl:
  void Add(int32_t a, int32_t b, const AddCallback& result) override {
    result(a + b);
  }

  FTL_DISALLOW_COPY_AND_ASSIGN(AdderImpl);
};

// Module implementation that acts as a recipe. There is one instance
// per application; the story runner creates new application instances
// to run more module instances.
class RecipeApp : public modular::SingleServiceViewApp<modular::Module> {
 public:
  RecipeApp() {}
  ~RecipeApp() override = default;

 private:
  // |SingleServiceViewApp|
  void CreateView(
      fidl::InterfaceRequest<mozart::ViewOwner> view_owner_request,
      fidl::InterfaceRequest<app::ServiceProvider> services) override {
    view_.reset(
        new RecipeView(application_context()
                           ->ConnectToEnvironmentService<mozart::ViewManager>(),
                       std::move(view_owner_request)));

    for (auto& view_owner : child_views_) {
      view_->ConnectView(std::move(view_owner));
    }

    child_views_.clear();
  }

  void ConnectView(fidl::InterfaceHandle<mozart::ViewOwner> view_owner) {
    if (view_) {
      view_->ConnectView(std::move(view_owner));
    } else {
      child_views_.emplace_back(std::move(view_owner));
    }
  }

  // |Module|
  void Initialize(
      fidl::InterfaceHandle<modular::Story> story,
      fidl::InterfaceHandle<modular::Link> link,
      fidl::InterfaceHandle<app::ServiceProvider> incoming_services,
      fidl::InterfaceRequest<app::ServiceProvider> outgoing_services) override {
    story_.Bind(std::move(story));
    link_.Bind(std::move(link));

    // Read initial Link data. We expect the shell to tell us what it
    // is.
    link_->Get(nullptr, [this](const fidl::String& json) {
      FTL_LOG(INFO) << "example_recipe link: " << json;
    });

    story_->CreateLink("module1", module1_link_.NewRequest());
    story_->CreateLink("module2", module2_link_.NewRequest());

    module1_link_->SetSchema(kJsonSchema);
    module2_link_->SetSchema(kJsonSchema);

    fidl::InterfaceHandle<modular::Link> module1_link_handle;
    module1_link_->Dup(module1_link_handle.NewRequest());

    fidl::InterfaceHandle<modular::Link> module2_link_handle;
    module2_link_->Dup(module2_link_handle.NewRequest());

    // Provide services for Module 1.
    app::ServiceProviderPtr services_for_module1;
    outgoing_services_.AddBinding(services_for_module1.NewRequest());
    outgoing_services_.AddService<modular::examples::Adder>(
        [this](fidl::InterfaceRequest<modular::examples::Adder> req) {
          adder_clients_.AddBinding(&adder_service_, std::move(req));
        });

    app::ServiceProviderPtr services_from_module1;
    fidl::InterfaceHandle<mozart::ViewOwner> module1_view;
    story_->StartModule(
        "file:///system/apps/example_module1", std::move(module1_link_handle),
        std::move(services_for_module1), services_from_module1.NewRequest(),
        module1_.NewRequest(), module1_view.NewRequest());
    ConnectView(std::move(module1_view));

    // Consume services from Module 1.
    auto multiplier_service =
        app::ConnectToService<modular::examples::Multiplier>(
            services_from_module1.get());
    multiplier_service.set_connection_error_handler([] {
      FTL_CHECK(false)
          << "Uh oh, Connection to Multiplier closed by the module 1.";
    });
    multiplier_service->Multiply(
        4, 4,
        ftl::MakeCopyable([multiplier_service =
                               std::move(multiplier_service)](int32_t result) {
          FTL_CHECK(result == 16);
          FTL_LOG(INFO) << "Incoming Multiplier service: 4 * 4 is 16.";
        }));

    fidl::InterfaceHandle<mozart::ViewOwner> module2_view;
    story_->StartModule("file:///system/apps/example_module2",
                        std::move(module2_link_handle), nullptr, nullptr,
                        module2_.NewRequest(), module2_view.NewRequest());
    ConnectView(std::move(module2_view));

    connections_.emplace_back(
        new LinkConnection(module1_link_.get(), module2_link_.get()));
    connections_.emplace_back(
        new LinkConnection(module2_link_.get(), module1_link_.get()));

    // Also connect with the root link, to create change notifications
    // the user shell can react on.
    connections_.emplace_back(
        new LinkConnection(module1_link_.get(), link_.get()));
    connections_.emplace_back(
        new LinkConnection(module2_link_.get(), link_.get()));

    module_monitors_.emplace_back(
        new ModuleMonitor(module1_.get(), story_.get()));
    module_monitors_.emplace_back(
        new ModuleMonitor(module2_.get(), story_.get()));

    // TODO(mesch): Good illustration of the remaining issue to
    // restart a story: Here is how does this code look like when
    // the Story is not new, but already contains existing Modules
    // and Links from the previous execution that is continued here.
    // Is that really enough?
    module1_link_->Get(nullptr, [this](const fidl::String& json) {
      if (json == "null") {
        // This must come last, otherwise LinkConnection gets a
        // notification of our own write because of the "send
        // initial values" code.
        std::vector<std::string> segments{modular_example::kJsonSegment,
                                          modular_example::kDocId};
        module1_link_->Set(fidl::Array<fidl::String>::From(segments),
                           kInitialJson);
      }
    });

    // This snippet of code demonstrates using the module's Ledger. Each time
    // this module is initialized, it updates a counter in the root page.
    // 1. Get the module's ledger.
    story_->GetLedger(module_ledger_.NewRequest(), [this](
                                                       ledger::Status status) {
      FTL_CHECK(status == ledger::Status::OK);
      // 2. Get the root page of the ledger.
      module_ledger_->GetRootPage(
          module_root_page_.NewRequest(), [this](ledger::Status status) {
            FTL_CHECK(status == ledger::Status::OK);
            // 3. Get a snapshot of the root page.
            module_root_page_->GetSnapshot(
                page_snapshot_.NewRequest(), nullptr,
                [this](ledger::Status status) {
                  FTL_CHECK(status == ledger::Status::OK);
                  // 4. Read the counter from the root page.
                  page_snapshot_->Get(
                      to_array(kLedgerCounterKey),
                      [this](ledger::Status status, ledger::ValuePtr value) {
                        // 5. If counter doesn't exist, initialize. Otherwise,
                        // increment.
                        if (status == ledger::Status::KEY_NOT_FOUND) {
                          FTL_LOG(INFO)
                              << "No counter in root page. Initializing to 1.";
                          fidl::Array<uint8_t> data;
                          data.push_back(1);
                          module_root_page_->Put(
                              to_array(kLedgerCounterKey), std::move(data),
                              [](ledger::Status status) {
                                FTL_CHECK(status == ledger::Status::OK);
                              });
                        } else {
                          FTL_CHECK(status == ledger::Status::OK);
                          FTL_CHECK(value->is_bytes());
                          fidl::Array<uint8_t> counter_data =
                              value->get_bytes().Clone();
                          FTL_LOG(INFO)
                              << "Retrieved counter from root page: "
                              << static_cast<uint32_t>(counter_data[0])
                              << ". Incrementing.";
                          counter_data[0]++;
                          module_root_page_->Put(
                              to_array(kLedgerCounterKey),
                              std::move(counter_data),
                              [](ledger::Status status) {
                                FTL_CHECK(status == ledger::Status::OK);
                              });
                        }
                      });
                });
          });
    });
  }

  // |Module|
  void Stop(const StopCallback& done) override {
    // TODO(mesch): This is tentative. Not sure what the right amount
    // of cleanup it is to ask from a module implementation, but this
    // disconnects all the watchers and thus prevents any further
    // state change of the module.
    connections_.clear();
    module_monitors_.clear();
    done();
  }

  std::unique_ptr<RecipeView> view_;
  std::vector<fidl::InterfaceHandle<mozart::ViewOwner>> child_views_;

  modular::LinkPtr link_;
  modular::StoryPtr story_;

  // This is a ServiceProvider we expose to one of our child modules, to
  // demonstrate the use of a service exchange.
  fidl::BindingSet<modular::examples::Adder> adder_clients_;
  AdderImpl adder_service_;
  app::ServiceProviderImpl outgoing_services_;

  // The following ledger interfaces are stored here to make life-time
  // management easier when chaining together lambda callbacks.
  ledger::LedgerPtr module_ledger_;
  ledger::PagePtr module_root_page_;
  ledger::PageSnapshotPtr page_snapshot_;

  modular::ModuleControllerPtr module1_;
  modular::LinkPtr module1_link_;

  modular::ModuleControllerPtr module2_;
  modular::LinkPtr module2_link_;

  std::vector<std::unique_ptr<LinkConnection>> connections_;
  std::vector<std::unique_ptr<ModuleMonitor>> module_monitors_;

  FTL_DISALLOW_COPY_AND_ASSIGN(RecipeApp);
};

}  // namespace

int main(int argc, const char** argv) {
  mtl::MessageLoop loop;
  RecipeApp app;
  loop.Run();
  return 0;
}
