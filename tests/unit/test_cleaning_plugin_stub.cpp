#include <gtest/gtest.h>

#include "cortrix/spc/data_cleaner.h"

// Cleaning S8: cleaning-plugin seam. cleaning gives the RegisterPlugin / ApplyPlugins /
// ICleaningPlugin signatures (D8 — cleaning plugin implements the full ecosystem); the
// seam is always compiled, these tests pin its stub behavior.
namespace cortrix::spc {
namespace {

Block MakeBlock(const std::string& id, const std::string& text) {
    Block b;
    b.id = id;
    b.chunk_text = text;
    b.embedding = {1.0f, 0.0f};
    return b;
}

// A registered lambda plugin runs over the batch (stub behavior — cleaning just
// invokes; cleaning plugin adds timeout/exception handling).
TEST(CleaningPluginStubTest, RegisteredPluginRuns) {
    DataCleaner c(CleaningConfig{});
    int invoked = 0;
    c.RegisterPlugin([&](std::vector<Block>& blocks) {
        ++invoked;
        for (Block& b : blocks) b.metadata_json["plugin.touched"] = true;
    });
    std::vector<Block> blocks = {MakeBlock("a", "x"), MakeBlock("b", "y")};
    c.ApplyPlugins(blocks);
    EXPECT_EQ(invoked, 1);
    EXPECT_TRUE(blocks[0].metadata_json["plugin.touched"]);
    EXPECT_TRUE(blocks[1].metadata_json["plugin.touched"]);
}

// Multiple plugins run in registration order.
TEST(CleaningPluginStubTest, PluginsRunInOrder) {
    DataCleaner c(CleaningConfig{});
    std::vector<int> order;
    c.RegisterPlugin([&](std::vector<Block>&) { order.push_back(1); });
    c.RegisterPlugin([&](std::vector<Block>&) { order.push_back(2); });
    std::vector<Block> blocks;
    c.ApplyPlugins(blocks);
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

// A concrete ICleaningPlugin subclass satisfies the stub interface (the shape
// Cleaning plugin will build on).
class TouchPlugin : public ICleaningPlugin {
public:
    const char* Name() const override { return "touch"; }
    void Apply(std::vector<Block>& blocks) override {
        for (Block& b : blocks) b.metadata_json["touch"] = true;
    }
};

TEST(CleaningPluginStubTest, ICleaningPluginInterfaceUsable) {
    TouchPlugin p;
    EXPECT_STREQ(p.Name(), "touch");
    std::vector<Block> blocks = {MakeBlock("a", "x")};
    p.Apply(blocks);
    EXPECT_TRUE(blocks[0].metadata_json["touch"]);
}

// The seam coexists with a default-config cleaner (config validation intact).
TEST(CleaningPluginStubTest, DefaultConfigValidates) {
    DataCleaner c(CleaningConfig{});
    EXPECT_TRUE(c.ValidateConfig().ok());
}

}  // namespace
}  // namespace cortrix::spc
