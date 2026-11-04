#pragma once

namespace gmod::app {

struct VehicleCamera {
    bool active = false;
    bool free_look = false;
    int player = -1;
    bool foot_first_person = false;

    void update(bool in_car, int followed, bool& first_person) {
        if (in_car) {
            if (!active) foot_first_person = first_person;
            if (!active || player != followed) free_look = false;
            active = true;
            player = followed;
            first_person = false;
        } else if (active) {
            active = false;
            player = -1;
            first_person = foot_first_person;
        }
    }
    void toggle(bool& first_person) {
        if (active) free_look = !free_look;
        else first_person = !first_person;
    }
};

} // namespace gmod::app
