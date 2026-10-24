// ACSI2TNFS v2-cable: 3D-printable case, one bottom and two lids.
//
//   lid        (print upside down) drops in behind the front panel
//   pcb        rests on a ledge along the edges, clear of the THT pins below
//   bottom     with the front panel, full height, flat
//   screw heads M2.5 countersunk from below, through bottom and PCB into plugs
//              that come down from the lid (the two 3 mm holes of the board)
//
// Lids:  "lid"     vertical box headers: the rear left corner is open, with walls
//                  around the headers that close off the rest of the case
//        "lid_ra"  right-angle box headers: openings in the rear wall
//
// The SD socket and the button go through the front panel: put the PCB in at the
// rear, slide it forward until the socket is flush with the panel, then screw it.
//
// Coordinates: X along the board from the left (KiCad x - 99.1), Y from the
// front edge to the rear edge (115.075 - KiCad y), Z = 0 on top of the PCB.
//
// openscad -D 'part="bottom"' -o acsi2tnfs-v2-cable-bottom.stl case.scad
//   (and part="lid", part="lid_ra"; "both" and "both_ra" show the assembly)

part = "both";

/* ---------- the board (from hardware/v2-cable/acsi2tnfs.kicad_pcb) ---------- */
pcb_w = 97.1;
pcb_d = 53.05;
pcb_t = 1.6;
holes = [[4.2, 39.575], [68.375, 12.1]];   // 3.0 mm

hdr_x = [[2.18, 35.18], [37.22, 70.22]];   // box header bodies J2, J1 (X)
hdr_y = 43.67;              // front of the box headers (Y); they reach the rear edge

/* ---------- what sticks out, adjust to your build ---------- */
pin_gap  = 3.0;             // room under the PCB for the THT pins
lid_in_h = 13.5;            // inside height of the lid above the PCB (Pico in a socket)

btn_x  = 34.58;             // angled button SW1: actuator centre
btn_z  = 3.5;               //   height of the actuator above the PCB
btn_d  = 4.0;               //   round opening; the actuator ends at Y -3.445

led_x  = [51.35, 59.86];    // D4, D3 (3 mm, bent forward, tip flush with the panel)
led_z  = 3.0;               //   height of the led centre above the PCB
led_d  = 3.2;

sd_x   = 10.54;             // micro SD socket Card1 (TF-01A): centre; its front
sd_w   = 14.74;             //   must be flush with the panel (KiCad y 107.895)
sd_h   = 1.85;

usb_x  = 82.79;             // Pico USB (centre of the Pico)
usb_z  = 10.6;              //   centre height above the PCB
usb_hole = [12, 8];

label   = "ACSI2TNFS";      // engraved in the front panel, bottom right corner
label_x = 96.6;             //   right end (X)
label_z = -2.0;             //   middle height (Z)
label_size = 4.0;
label_font = "Liberation Sans:style=Bold";
label_depth = 0.6;

ra_h   = 9.3;               // lid_ra: height of the openings for the right-angle headers
ra_rib = 8;                 //   triangular rib behind the post between them: length

/* ---------- the case ---------- */
wall  = 2.0;
floor_t = 2.4;
roof_t  = 2.0;
clr   = 0.5;                // play around the PCB
front_gap = 0.4;            // between the PCB edge and the front panel
slide = 3.6;                // room at the rear to slide the PCB in (socket and button)
ledge = 1.0;                // the PCB rests on this much of its edge
step  = 1.5;                // lid and bottom overlap (rabbet) this high, under the PCB top
rib_x = [3.6, 36.2, 69.5];  // small walls under the rear edge, between the header pins
lid_gap = 0.2;              // between the lid and the front panel
plug_d = 6.0;
pilot  = 2.2;               // M2.5 self-tapping into the plug
screw_d = 2.8;              // M2.5 through the bottom and the PCB
head_d  = 5.2;              // M2.5 countersunk head
pw     = 1.5;               // lid: walls around the box headers
pk_x   = hdr_x[1][1] + 0.25; //   inside of the pocket (X) ...
pk_y   = hdr_y - 0.3;       //   ... and (Y)

in_x0 = -clr;               in_x1 = pcb_w + clr;
in_y0 = -front_gap;         in_y1 = pcb_d + slide;
z_floor = -pcb_t - pin_gap; // top of the floor
z_bot   = z_floor - floor_t;
z_top   = lid_in_h + roof_t;
o_x0 = in_x0 - wall;        o_x1 = in_x1 + wall;
o_y0 = in_y0 - wall;        o_y1 = in_y1 + wall;

$fn = 48;

module box(x0, y0, z0, x1, y1, z1) { translate([x0, y0, z0]) cube([x1 - x0, y1 - y0, z1 - z0]); }

module outer(z0, z1, grow = 0) { box(o_x0 - grow, o_y0 - grow, z0, o_x1 + grow, o_y1 + grow, z1); }
module inner(z0, z1, grow = 0) { box(in_x0 + grow, in_y0 + grow, z0, in_x1 - grow, in_y1 - grow, z1); }

module bottom() {
    difference() {
        union() {
            difference() {
                outer(z_bot, -step);
                inner(z_floor, 1);
            }
            // outer half of the wall goes up to the PCB top: the lid's inner half fits behind it
            difference() { outer(-step - 0.01, 0); outer(-step - 1, 1, -wall / 2); }
            // front panel, full height
            box(o_x0, o_y0, z_bot, o_x1, in_y0, z_top);
            // ledge along the front and the sides; at the rear only small walls,
            // so the THT pins pass while the PCB slides in
            difference() {
                inner(z_floor - 0.1, -pcb_t);
                box(ledge, ledge, z_floor - 1, pcb_w - ledge, in_y1 + 1, 0);
            }
            for (x = rib_x) box(x - 0.6, pcb_d - 2, z_floor - 0.1, x + 0.6, in_y1, -pcb_t);
            // pillars under the screw holes
            for (h = holes) translate([h.x, h.y, z_floor - 0.1]) cylinder(d = plug_d, h = pin_gap + 0.1);
        }
        for (h = holes) {
            translate([h.x, h.y, z_bot - 1]) cylinder(d = screw_d, h = 20);
            translate([h.x, h.y, z_bot - 0.01]) cylinder(d1 = head_d, d2 = screw_d, h = (head_d - screw_d) / 2);
        }
        // front panel: button, leds, SD socket
        translate([btn_x, in_y0 + 1, btn_z]) rotate([90, 0, 0]) cylinder(d = btn_d, h = wall + 2);
        for (x = led_x) translate([x, in_y0 + 1, led_z]) rotate([90, 0, 0]) cylinder(d = led_d, h = wall + 2);
        box(sd_x - sd_w / 2 - 0.2, o_y0 - 1, -0.1, sd_x + sd_w / 2 + 0.2, in_y0 + 1, sd_h + 0.2);
        translate([label_x, o_y0 + label_depth, label_z]) rotate([90, 0, 0])
            linear_extrude(label_depth + 1)
                text(label, size = label_size, font = label_font, halign = "right", valign = "center");
    }
}

module lid(ra) {
    difference() {
        union() {
            difference() {
                outer(0, z_top);
                inner(-1, lid_in_h);
            }
            // inner half of the wall reaches down past the PCB edge
            difference() { outer(-step, 0.01, -wall / 2 - 0.15); inner(-step - 1, 1); }
            // plugs down to the PCB at the screw holes
            for (h = holes) translate([h.x, h.y, 0]) cylinder(d = plug_d, h = lid_in_h + 0.1);
            // walls in front of and to the right of the box headers
            if (!ra) {
                box(o_x0, pk_y - pw, 0, pk_x + pw, pk_y, z_top);
                box(pk_x, pk_y - pw, 0, pk_x + pw, o_y1, z_top);
            }
            // rib behind the post between the right-angle headers
            if (ra) hull() {
                box(hdr_x[0][1] + 0.4, in_y1 - 0.01, 0, hdr_x[1][0] - 0.4, in_y1 + 0.5, lid_in_h + 0.01);
                box(hdr_x[0][1] + 0.4, in_y1 - ra_rib, lid_in_h, hdr_x[1][0] - 0.4, in_y1 + 0.5, lid_in_h + 0.01);
            }
        }
        // no front: the lid stops behind the front panel
        box(o_x0 - 1, o_y0 - 1, -step - 1, o_x1 + 1, in_y0 + lid_gap, z_top + 1);
        for (h = holes) translate([h.x, h.y, -1]) cylinder(d = pilot, h = 11);
        // rear: Pico USB
        box(usb_x - usb_hole[0] / 2, in_y1 - 1, usb_z - usb_hole[1] / 2,
            usb_x + usb_hole[0] / 2, o_y1 + 1, min(usb_z + usb_hole[1] / 2, lid_in_h));
        if (ra) for (h = hdr_x) box(h[0] - 0.4, in_y1 - 1, -step - 1, h[1] + 0.4, o_y1 + 1, ra_h);
        else box(o_x0 - 1, pk_y, -step - 1, pk_x, o_y1 + 1, z_top + 1);
    }
}

module lid_print(ra) { translate([0, 0, z_top]) rotate([180, 0, 0]) lid(ra); }

module assembly(ra) {
    color("SteelBlue") bottom();
    color("Khaki", 0.6) lid(ra);
    color("Green") box(0, 0, -pcb_t, pcb_w, pcb_d, 0);
}

if (part == "bottom") bottom();
else if (part == "lid") lid_print(false);
else if (part == "lid_ra") lid_print(true);
else if (part == "both_ra") assembly(true);
else assembly(false);
