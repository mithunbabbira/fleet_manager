// Truck dashboard enclosure for ESP32-C6, EC200U and XY-3606.
// OpenSCAD-compatible ASCII source. Units are millimetres.
//
// render_mode = 0 : show all printable parts
// render_mode = 1 : show base only
// render_mode = 2 : show lid only
// render_mode = 3 : show ESP32 mezzanine only

$fn = 40;
render_mode = 0;

case_x = 122;
case_y = 82;
base_height = 30;
wall = 2.5;
floor_thickness = 3;
lid_thickness = 4;

module vertical_slot(length, width, height) {
    hull() {
        translate([0, -length / 2 + width / 2, 0])
            cylinder(d = width, h = height);
        translate([0, length / 2 - width / 2, 0])
            cylinder(d = width, h = height);
    }
}

module base_shell() {
    difference() {
        cube([case_x, case_y, base_height]);
        translate([wall, wall, floor_thickness])
            cube([
                case_x - 2 * wall,
                case_y - 2 * wall,
                base_height
            ]);
    }
}

module dashboard_ears() {
    // Four 10 mm mounting ears. Overall width is 142 mm.
    translate([-10, 7, 0])
        cube([10, 20, 4]);
    translate([-10, case_y - 27, 0])
        cube([10, 20, 4]);
    translate([case_x, 7, 0])
        cube([10, 20, 4]);
    translate([case_x, case_y - 27, 0])
        cube([10, 20, 4]);
}

module lid_bosses() {
    for (x = [8, case_x - 8]) {
        for (y = [8, case_y - 8]) {
            translate([x, y, floor_thickness])
                cylinder(d = 8, h = base_height - floor_thickness);
        }
    }
}

module board_posts() {
    // Lower EC200U carrier supports.
    for (x = [8, 80]) {
        for (y = [14, 66]) {
            translate([x, y, floor_thickness])
                cylinder(d = 6, h = 5);
        }
    }

    // Lower XY-3606 supports.
    for (x = [90, 114]) {
        for (y = [12, 70]) {
            translate([x, y, floor_thickness])
                cylinder(d = 6, h = 5);
        }
    }

    // Tall supports for the ESP32-C6 mezzanine.
    for (x = [17, 73]) {
        for (y = [20, 51]) {
            translate([x, y, floor_thickness])
                cylinder(d = 7, h = 16);
        }
    }
}

module base_cutouts() {
    // Four M4 dashboard mounting slots.
    for (x = [-5, case_x + 5]) {
        for (y = [17, case_y - 17]) {
            translate([x, y, -0.1])
                vertical_slot(10, 4.8, 4.2);
        }
    }

    // Four M3 self-tapping lid screw pilot holes.
    for (x = [8, case_x - 8]) {
        for (y = [8, case_y - 8]) {
            translate([x, y, floor_thickness - 0.1])
                cylinder(d = 2.7, h = base_height);
        }
    }

    // Pilot holes in all board posts.
    for (x = [8, 80]) {
        for (y = [14, 66]) {
            translate([x, y, floor_thickness - 0.1])
                cylinder(d = 2.4, h = 5.2);
        }
    }
    for (x = [90, 114]) {
        for (y = [12, 70]) {
            translate([x, y, floor_thickness - 0.1])
                cylinder(d = 2.4, h = 5.2);
        }
    }
    for (x = [17, 73]) {
        for (y = [20, 51]) {
            translate([x, y, floor_thickness - 0.1])
                cylinder(d = 2.4, h = 16.2);
        }
    }

    // M16 truck-power cable gland opening on the left wall.
    translate([-0.1, 35, 16])
        rotate([0, 90, 0])
            cylinder(d = 16, h = wall + 0.2);

    // USB-C service opening on the right wall.
    translate([case_x - wall - 0.1, 25, 12])
        cube([wall + 0.2, 14, 8]);

    // SMA LTE antenna bulkhead opening on the right wall.
    translate([case_x - wall - 0.1, 61, 17])
        rotate([0, 90, 0])
            cylinder(d = 6.5, h = wall + 0.2);

    // Side ventilation over the power converter.
    for (x = [88, 95, 102, 109, 116]) {
        translate([x, case_y - wall - 0.1, 19])
            cube([3, wall + 0.2, 7]);
    }
}

module base() {
    difference() {
        union() {
            base_shell();
            dashboard_ears();
            lid_bosses();
            board_posts();
        }
        base_cutouts();
    }
}

module lid() {
    difference() {
        union() {
            // The outside face is on the build plate.
            cube([case_x, case_y, lid_thickness]);

            // Internal locating rim.
            translate([3, 3, lid_thickness])
                difference() {
                    cube([case_x - 6, case_y - 6, 4]);
                    translate([2, 2, -0.1])
                        cube([case_x - 10, case_y - 10, 4.2]);
                }
        }

        // M3 screw clearances and head counterbores.
        for (x = [8, case_x - 8]) {
            for (y = [8, case_y - 8]) {
                translate([x, y, -0.1])
                    cylinder(d = 3.2, h = 8.2);
                translate([x, y, -0.1])
                    cylinder(d = 6.2, h = 2);
            }
        }

        // Top ventilation slots above XY-3606.
        for (y = [18, 25, 32, 39, 46, 53, 60]) {
            translate([88, y, -0.1])
                cube([24, 3, lid_thickness + 0.2]);
        }
    }
}

module mezzanine() {
    difference() {
        cube([62, 36, 2.4]);

        // Four holes align with the tall base posts.
        for (x = [3, 59]) {
            for (y = [2.5, 33.5]) {
                translate([x, y, -0.1])
                    cylinder(d = 2.8, h = 2.6);
            }
        }

        // Cable-tie slots for ESP32-C6 boards without mounting holes.
        for (x = [9, 50]) {
            for (y = [7, 22]) {
                translate([x, y, -0.1])
                    cube([3, 7, 2.6]);
            }
        }
    }

    // Low side guides for the ESP32-C6 board.
    translate([5, 3, 2.4])
        cube([52, 1.5, 2]);
    translate([5, 31.5, 2.4])
        cube([52, 1.5, 2]);
}

if (render_mode == 0) {
    base();
    translate([155, 0, 0])
        lid();
    translate([185, 100, 0])
        mezzanine();
}

if (render_mode == 1) {
    base();
}

if (render_mode == 2) {
    lid();
}

if (render_mode == 3) {
    mezzanine();
}
