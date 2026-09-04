#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <thread>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/overlay/achievement_toast.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

namespace {
class TestContext final : public rex::ui::WindowedAppContext {
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};

// No SDL initialization, native window, presenter, or GPU is needed.
class TestWindow final : public rex::ui::Window {
 public:
  explicit TestWindow(TestContext& context) : Window(context, "dialog test", 640, 480) {}
  ~TestWindow() override { EnterDestructor(); }

 private:
  bool OpenImpl() override { return true; }
  void RequestCloseImpl() override {}
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(rex::ui::Surface::TypeFlags) override {
    return nullptr;
  }
  void RequestPaintImpl() override {}
};

class TestTexture final : public rex::ui::ImmediateTexture {
 public:
  TestTexture(uint32_t width, uint32_t height) : ImmediateTexture(width, height) {}
};

class TestImmediateDrawer final : public rex::ui::ImmediateDrawer {
 public:
  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                           rex::ui::ImmediateTextureFilter, bool,
                                                           const uint8_t*) override {
    return std::make_unique<TestTexture>(width, height);
  }
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch&) override {}
  void Draw(const rex::ui::ImmediateDraw&) override {}
  void EndDrawBatch() override {}
};

class TestDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit TestDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  int draws = 0;
  bool hide_during_draw = false;

 private:
  void OnDraw(ImGuiIO&) override {
    ++draws;
    if (hide_during_draw)
      SetVisible(false);
  }
};

struct Fixture {
  TestContext context;
  TestWindow window{context};
  TestImmediateDrawer immediate;
  rex::ui::ImGuiDrawer drawer{&window, 0};
  rex::ui::AppUIDrawContext draw_context{640, 480};

  Fixture() { drawer.SetImmediateDrawer(&immediate); }
  ~Fixture() { drawer.SetImmediateDrawer(nullptr); }
  void Draw() { drawer.Draw(draw_context); }
};
}  // namespace

TEST_CASE("ImGui persistent dialog visibility controls real drawer dispatch", "[ui][dialog]") {
  Fixture fixture;
  TestDialog dialog(&fixture.drawer);
  REQUIRE(dialog.is_visible());
  fixture.Draw();
  REQUIRE(dialog.draws == 1);
  dialog.SetVisible(false);
  dialog.SetVisible(false);
  fixture.Draw();
  REQUIRE(dialog.draws == 1);
  dialog.SetVisible(true);
  dialog.SetVisible(true);
  fixture.Draw();
  REQUIRE(dialog.draws == 2);
}

TEST_CASE("ImGui self-hiding dialog does not skip the next dialog", "[ui][dialog]") {
  Fixture fixture;
  TestDialog first(&fixture.drawer);
  TestDialog second(&fixture.drawer);
  first.hide_during_draw = true;
  fixture.Draw();
  REQUIRE_FALSE(first.is_visible());
  REQUIRE(first.draws == 1);
  REQUIRE(second.draws == 1);
  fixture.Draw();
  REQUIRE(first.draws == 1);
  REQUIRE(second.draws == 2);
  second.SetVisible(false);
  fixture.Draw();
  REQUIRE(second.draws == 2);
  first.SetVisible(true);
  fixture.Draw();
  REQUIRE(first.draws == 2);
  REQUIRE_FALSE(first.is_visible());
}

TEST_CASE("ImGui worker visibility requests run only on the UI queue", "[ui][dialog]") {
  Fixture fixture;
  TestDialog dialog(&fixture.drawer);
  dialog.SetVisible(false);
  std::thread worker([&] { dialog.RequestVisibility(true); });
  worker.join();
  REQUIRE_FALSE(dialog.is_visible());
  fixture.Draw();
  REQUIRE(dialog.draws == 0);
  fixture.context.ExecutePendingFunctionsFromUIThread();
  REQUIRE(dialog.is_visible());
  fixture.Draw();
  REQUIRE(dialog.draws == 1);
  dialog.RequestVisibility(false);
  dialog.RequestVisibility(true);
  dialog.RequestVisibility(false);
  fixture.context.ExecutePendingFunctionsFromUIThread();
  REQUIRE_FALSE(dialog.is_visible());
}

TEST_CASE("ImGui queued visibility is harmless after dialog destruction", "[ui][dialog]") {
  Fixture fixture;
  auto dialog = std::make_unique<TestDialog>(&fixture.drawer);
  std::thread worker([&] { dialog->RequestVisibility(true); });
  worker.join();
  dialog.reset();
  fixture.context.ExecutePendingFunctionsFromUIThread();
  TestDialog survivor(&fixture.drawer);
  fixture.Draw();
  REQUIRE(survivor.draws == 1);
}

TEST_CASE("Achievement toast is hidden until a worker queues an event", "[ui][dialog]") {
  Fixture fixture;
  rex::ui::AchievementToastDialog toast(&fixture.drawer, nullptr, nullptr);
  REQUIRE_FALSE(toast.is_visible());
  // A stale show request with an empty queue must detach on the next draw.
  toast.SetVisible(true);
  fixture.Draw();
  REQUIRE_FALSE(toast.is_visible());
  std::thread worker([&] { toast.Push(rex::system::AchievementEvent{}); });
  worker.join();
  REQUIRE_FALSE(toast.is_visible());
  fixture.context.ExecutePendingFunctionsFromUIThread();
  REQUIRE(toast.is_visible());
  fixture.Draw();
  REQUIRE(toast.is_visible());
}
