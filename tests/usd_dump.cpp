/*
    usd_dump.cpp -- Developer tool: prints a USD layer (.usda / .usdc /
    .usdz) as read by Instant Meshes, in the canonical form that
    tests/usd_reference.py prints with Pixar's USD library, so that both
    can be compared line by line.

    Usage: usd_dump [--stage] file.usd
    --stage: the composed stage (usdstage.h) instead of the layer
*/

#include "usd.h"
#include "usdstage.h"
#include <cstdio>
#include <algorithm>
#include <cmath>

using namespace usd;

static std::string fmt(double x) {
    if (std::abs(x) < 5e-5)
        x = 0;
    char b[64];
    snprintf(b, sizeof b, "%.4g", x);
    return b;
}

/* count, sum and first values of the flattened numbers; strings joined */
static std::string summary(const Value &v) {
    switch (v.kind) {
        case Value::Empty: return "-";
        case Value::Blocked: return "None";
        case Value::Numbers: {
            double sum = 0;
            for (double x : v.numbers)
                sum += x;
            std::string s = "n=" + std::to_string(v.numbers.size()) + " sum=" + fmt(sum) + " [";
            for (size_t k = 0; k < v.numbers.size() && k < 4; ++k)
                s += (k ? " " : "") + fmt(v.numbers[k]);
            return s + "]";
        }
        case Value::Strings: {
            std::string s = "s" + std::to_string(v.strings.size()) + " [";
            for (size_t k = 0; k < v.strings.size() && k < 4; ++k)
                s += (k ? " | " : "") + v.strings[k];
            return s + "]";
        }
        case Value::ListOp: {
            std::vector<std::string> items = v.list_items();
            std::string s = "list" + std::to_string(items.size()) + " [";
            for (size_t k = 0; k < items.size() && k < 4; ++k)
                s += (k ? " | " : "") + items[k];
            return s + "]";
        }
        default:
            return "?";
    }
}

static void dump(const Layer &layer, const Prim &p) {
    if (p.path != "/") {
        const char *spec = p.specifier == Specifier::Def ? "def" : (p.specifier == Specifier::Over ? "over" : "class");
        printf("PRIM %s %s %s\n", p.path.c_str(), spec, p.type.c_str());
        std::vector<const Property *> props;
        for (const Property &q : p.properties)
            props.push_back(&q);
        std::sort(props.begin(), props.end(), [](const Property *a, const Property *b) { return a->name < b->name; });
        for (const Property *q : props) {
            if (q->relationship) {
                std::string t;
                for (const std::string &x : q->targets)
                    t += (t.empty() ? "" : " | ") + x;
                printf("  REL %s [%s]\n", q->name.c_str(), t.c_str());
            } else {
                printf("  ATTR %s %s%s = %s%s\n", q->name.c_str(), q->uniform ? "uniform " : "", q->type.c_str(),
                       summary(layer.value(*q)).c_str(), q->hasTimeSamples ? " (timeSamples)" : "");
                if (q->hasTimeSamples)
                    for (const auto &sample : layer.samples(*q).samples)
                        printf("    SAMPLE %s: %s\n", fmt(sample.first).c_str(), summary(sample.second).c_str());
            }
        }
    }
    for (const auto &c : p.children)
        dump(layer, *c);
}

int main(int argc, char **argv) {
    const bool stage = argc == 3 && std::string(argv[1]) == "--stage";
    if (argc != 2 && !stage) {
        fprintf(stderr, "Usage: usd_dump [--stage] file.usd\n");
        return 2;
    }
    try {
        std::shared_ptr<const Layer> opened = stage ? open_stage(argv[2]) : std::make_shared<const Layer>(argv[1]);
        const Layer &layer = *opened;
        printf("FORMAT %s\n", layer.format().c_str());
        for (const char *key : { "upAxis", "metersPerUnit", "defaultPrim" }) {
            auto it = layer.meta.find(key);
            if (it != layer.meta.end())
                printf("META %s = %s\n", key, summary(it->second).c_str());
        }
        dump(layer, layer.root);
    } catch (const std::exception &e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
    return 0;
}
