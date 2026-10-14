// VanRelay enclosure: ELEGOO ESP32 DevKit (ESP-WROOM-32, USB-C) beside a Valefod 1-channel relay module.
// Render: openscad -D 'part="base"' -o stl/relay-base.stl relay-case.scad   (and part="lid")
// Values marked ESTIMATE were not measured: check them before printing.
part = "both";              // "base", "lid", "both"

esp      = [51, 28.5, 1.3]; // PCB length (USB shell sticks out esp_usb_out more), width = ESTIMATE, thickness
esp_usb_out = 2;            // USB-C shell protrudes this far past the PCB end
esp_pin  = 10 - 1.3;        // pins hang below the PCB: 10 mm from the board top to the pin tips
esp_rows = 25.5;            // ESTIMATE: distance between the two pin rows (pedestal stays narrower)
esp_top  = 5;               // ESTIMATE: tallest part above the PCB (module can, buttons)
usb_w = 11; usb_h = 5.5;    // USB-C opening (shell is ~9 x 3.3 mm, extra for the plug overmould)

rel      = [51, 26, 19];    // relay module overall L x W x H (measured), screw terminals on both short ends
rel_post = 4;               // standoff height under the relay PCB
rel_pcb  = 1.6;
rel_hole = 2.44;            // hole centres from the PCB edges (measured)
rel_hole_d = 3;             // PCB hole diameter (measured): M3 screws, self-tapping into the standoffs
rel_term_h = 11;            // terminal block height above the PCB (wire entry slot height)

wall = 2.4; gap = 1.0; floor_t = 2.4;
post_d = 6; split = 6;      // the divider between the boards carries the two lid-screw posts
screw_d = 3.2;              // lid screw clearance; posts take an M3 self-tapping screw (2.6 mm pilot)
ear = 8;

esp_x = wall + 1;           // USB tip sits 1 mm inside the wall opening (PCB end = tip + esp_usb_out)
rel_x = wall + gap;

inner_l = max(esp_x - wall + esp[0], rel_x - wall + rel[0]) + gap;
inner_w = esp[1] + rel[1] + split + 4*gap;
inner_h = max(esp_pin + esp[2] + esp_top, rel_post + rel_pcb + rel[2]) + 2;
L = inner_l + 2*wall; W = inner_w + 2*wall; H = inner_h + floor_t;

esp_y = wall + gap;
div_y = esp_y + esp[1] + gap;          // divider strip start
rel_y = div_y + split + gap;
echo(str("Outer size: ", L, " x ", W, " x ", H, " mm"));

module post(x, y, h, d, hole) {
    translate([x, y, 0]) difference() { cylinder(d = d, h = h, $fn = 32); translate([0,0,1]) cylinder(d = hole, h = h, $fn = 24); }
}
post_x = [wall + post_d/2, L - wall - post_d/2];
post_y = div_y + split/2;

module base() {
    difference() {
        union() {
            difference() {
                cube([L, W, H]);
                translate([wall, wall, floor_t]) cube([inner_l, inner_w, H]);
            }
            for (y = [-ear, W]) translate([L/2 - 6, y, 0]) cube([12, ear, floor_t]);
            // ESP32 pedestal between the pin rows, plus an end stop so it can't slide away from the USB opening
            translate([esp_x, esp_y + (esp[1] - (esp_rows - 4))/2, floor_t]) cube([esp[0], esp_rows - 4, esp_pin]);
            translate([esp_x + esp[0] + 0.3, esp_y, floor_t]) cube([1.6, esp[1], esp_pin + esp[2] + 2]);
            // divider rib between the boards, with the lid-screw posts at both ends
            translate([wall, post_y - 1, floor_t]) cube([inner_l, 2, esp_pin + esp[2]]);
            for (x = post_x) post(x, post_y, H - 0.2, post_d, 2.6);
            // relay standoffs under the four mounting holes
            for (x = [rel_hole, rel[0] - rel_hole], y = [rel_hole, rel[1] - rel_hole])
                post(rel_x + x, rel_y + y, rel_post, 5, 2.6);
        }
        // USB-C opening, centred on the ESP32 width, in the -X wall
        translate([-1, esp_y + esp[1]/2 - usb_w/2, floor_t + esp_pin + esp[2] - 0.5]) cube([wall + 2, usb_w, usb_h]);
        // relay screw terminals face both end walls: wire slots on both, just above the PCB
        for (x = [-1, L - wall - 1])
            translate([x, rel_y + 2, floor_t + rel_post + rel_pcb + 1]) cube([wall + 2, rel[1] - 4, rel_term_h - 2]);
        // power lead exit for the ESP32 side, +X end
        translate([L - wall - 1, esp_y + esp[1]/2 - 7, floor_t + 2]) cube([wall + 2, 14, 8]);
        for (y = [-ear/2, W + ear/2]) translate([L/2, y, -1]) cylinder(d = 4.2, h = floor_t + 2, $fn = 24);
    }
}

module lid() {
    difference() {
        union() {
            cube([L, W, 2]);
            translate([wall + 0.3, wall + 0.3, 2]) difference() {
                cube([inner_l - 0.6, inner_w - 0.6, 2]);
                translate([2, 2, -1]) cube([inner_l - 4.6, inner_w - 4.6, 4]);
            }
            // pegs that press the ESP32 down onto its pedestal
            for (y = [esp_y + 6, esp_y + esp[1] - 8]) translate([esp_x + esp[0] - 10, y, 2])
                cube([4, 2, 2 + inner_h - (esp_pin + esp[2] + esp_top) - 0.5]);
        }
        for (x = post_x) translate([x, post_y, -1]) cylinder(d = screw_d, h = 5, $fn = 24);
        for (i = [0:5]) translate([esp_x + 8 + i*7, esp_y + 5, -1]) cube([3, esp[1] - 10, 4]);
        for (i = [0:5]) translate([rel_x + 8 + i*7, rel_y + 5, -1]) cube([3, rel[1] - 10, 4]);
    }
}

if (part == "base" || part == "both") base();
if (part == "lid"  || part == "both") translate([0, W + 2*ear + 6, 0]) lid();
