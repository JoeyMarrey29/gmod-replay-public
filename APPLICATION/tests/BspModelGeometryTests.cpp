#include "source/bsp/BspModelGeometry.h"
#include "source/bsp/BspReader.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr std::size_t ModelSize = 48;

void set_face_range(std::vector<std::uint8_t>& models, std::size_t index,
                    std::int32_t first, std::int32_t count) {
    std::memcpy(models.data() + index * ModelSize + 40, &first, sizeof(first));
    std::memcpy(models.data() + index * ModelSize + 44, &count, sizeof(count));
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    using gmod::source::bsp::model_face_range;
    using namespace gmod::source::bsp;
    require(map_creation_id(0) == 1235 && map_creation_id(163) == 1398,
            "MapCreationIDs must include the one-based entity-lump ordinal.");
    std::vector<EntityModel> scenes(6);
    scenes[0].targetname = scenes[1].targetname = "scenesystem28";
    scenes[0].map_created_id = 1398;
    scenes[1].map_created_id = 1400;
    scenes[2].targetname = "scenesystem21";
    scenes[2].map_created_id = 5329;
    scenes[3].targetname = "scenesystem_case";
    scenes[4].targetname = "ordinary_cone";
    scenes[5].targetname = "scenesystem";
    require(!unrecorded_map_scenes(scenes, {}),
            "Legacy recordings without map IDs must report unavailable scene selection.");
    const std::vector<int> recorded{1235, 1398, 1398};
    const auto inactive = unrecorded_map_scenes(scenes, recorded);
    require(inactive && *inactive == std::set<std::string>{"scenesystem21"},
            "Recorded scenes must retain all members without filtering ordinary props or logic.");
    const std::vector<int> other_recording{1235, 5329};
    require(*unrecorded_map_scenes(scenes, other_recording) == std::set<std::string>{"scenesystem28"},
            "Scene selection leaked between recordings.");
    require(*unrecorded_map_scenes(scenes, recorded) == *inactive,
            "Returning to a recording changed its startup scene selection.");

    std::vector<std::uint8_t> models(5 * ModelSize);
    set_face_range(models, 0, 0, 10);
    set_face_range(models, 1, 10, 0);
    set_face_range(models, 2, 10, 0);
    set_face_range(models, 3, 16, 0);
    set_face_range(models, 4, 19, 1);

    const auto empty_start = model_face_range(models, 1, 20);
    const auto empty_shared_start = model_face_range(models, 2, 20);
    const auto empty_before_positive = model_face_range(models, 3, 20);
    const auto explicit_range = model_face_range(models, 4, 20);
    require(!empty_start && !empty_shared_start && !empty_before_positive,
            "Zero-count BSP model inferred or stole a neighboring face range.");
    require(explicit_range && explicit_range->first == 19 && explicit_range->count == 1,
            "Explicit model face range was not preserved.");
    const auto world_range = model_face_range(models, 0, 20);
    require(world_range && world_range->first == 0 && world_range->count == 10,
            "Positive world model face range was not preserved exactly.");
    require(!model_face_range(models, 4, 19),
            "Out-of-range explicit model face range was accepted.");

    models.pop_back();
    require(!model_face_range(models, 1, 20), "Misaligned model lump was accepted.");

    std::cout << "PASS: exact BSP face ranges, map IDs and recorded startup scene groups\n";
}
