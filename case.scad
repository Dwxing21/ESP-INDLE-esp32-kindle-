// ============================================================
// ESPINDLE Case — parametric OpenSCAD source
// ============================================================
// MEASURE YOUR OWN BOARDS WITH CALIPERS AND ADJUST THE VALUES
// BELOW BEFORE PRINTING. The defaults are typical sizes for a
// generic 2.8" ILI9341 SPI+SD breakout and an ESP32-S3-DevKitC-1
// style board, but real dimensions vary between suppliers.
// ============================================================

// ---------- Which part to render ----------
// "base" = bottom shell (holds the boards)
// "lid"  = top shell (screen window + buttons)
// "both" = both, side by side (preview only, don't print this way)
PART = "both"; // [base, lid, both]

// ---------- General ----------
wall        = 2.2;   // outer wall thickness
floor_t     = 2.0;   // base floor thickness
lid_t       = 2.0;   // lid thickness
gap         = 0.3;   // clearance between base walls and lid
corner_r    = 4;     // outer corner rounding radius

// ---------- TFT board (2.8" ILI9341 + SD) ----------
tft_w       = 54;    // board width (short axis)
tft_h       = 78;    // board length (long axis, includes header strip)
tft_thick   = 3.2;   // board PCB + connector thickness, for standoff height
tft_active_w = 43;   // visible screen width
tft_active_h = 58;   // visible screen height
tft_active_off_x = (tft_w - tft_active_w) / 2; // horizontal centering — adjust if off-center on your board
tft_active_off_y = 8;                          // gap from top edge of board to top of visible screen — MEASURE THIS
tft_mount_inset  = 3; // distance from board edge to its mounting-hole centers (adjust to your board's holes)
tft_hole_dia     = 2.4; // screw/standoff hole diameter for TFT board

// ---------- ESP32-S3 DevKitC-1 style board ----------
esp_w       = 25.4;
esp_h       = 63.2;
esp_thick   = 8;     // clearance height needed under/around the board (USB connector, headers)
esp_mount_inset = 3;
esp_hole_dia    = 2.6;

// ---------- Buttons (3x, front edge of lid, thumb reach) ----------
btn_hole_dia = 6.0;
btn_spacing  = 16;

// ---------- Print bed ----------
// da Vinci mini's actual spec'd build volume is 150x150x150mm — designing
// to this (with a small margin) fits both that and a 160x160 bed.
max_plate_xy = 148;

// ---------- Layout (side-by-side, not stacked — keeps the case short) ----------
column_gap    = 6;    // horizontal gap between the TFT and ESP32 columns, for wiring
button_zone_h = 16;   // strip reserved below the TFT board for buttons
lip_frame_w   = 2.5;  // width of the lid's thin alignment rim — must NOT cover the interior

inner_w = tft_w + column_gap + esp_w;
inner_h = tft_h + button_zone_h;                      // tallest column (TFT) + its button strip
inner_d = max(tft_thick, esp_thick) + 6;               // boards are side-by-side, so depth = the taller one, not the sum

outer_w = inner_w + 2 * wall;
outer_h = inner_h + 2 * wall;
outer_d = inner_d + floor_t + lid_t;

// Standoff post height for TFT board (raises its top surface near the lid window)
tft_standoff_h = inner_d - tft_thick - 1;
// Standoff post height for ESP32 board (mounted lower, doesn't need to reach the lid)
esp_standoff_h = 4;

echo(str("Overall case size (mm): ", outer_w, " x ", outer_h, " x ", outer_d));
if (outer_w > max_plate_xy || outer_h > max_plate_xy) {
  echo(str("*** WARNING: case footprint exceeds ", max_plate_xy, "mm — will NOT fit the print bed! ***"));
}

// ============================================================
// Helper modules
// ============================================================
module rounded_rect(w, h, r) {
  hull() {
    for (x = [r, w - r])
      for (y = [r, h - r])
        translate([x, y, 0]) circle(r = r, $fn = 32);
  }
}

module standoff(hole_dia, height) {
  difference() {
    cylinder(d = hole_dia + 4, h = height, $fn = 24);
    translate([0, 0, -0.5]) cylinder(d = hole_dia, h = height + 1, $fn = 16);
  }
}

// corner screw bosses joining base + lid (self-tapping screws, e.g. M2.5)
module corner_bosses(w, h, height, hole_dia = 2.2, inset = 6) {
  positions = [[inset, inset], [w - inset, inset], [inset, h - inset], [w - inset, h - inset]];
  for (p = positions)
    translate([p[0], p[1], 0])
      standoff(hole_dia, height);
}

// ============================================================
// BASE (bottom shell)
// ============================================================
module case_base() {
  // Both boards top-aligned to the same interior top edge; TFT column has a
  // button strip below it, ESP32 column has empty space below (for wiring/battery).
  esp_x = wall;
  tft_x = wall + esp_w + column_gap;
  tft_y = wall + button_zone_h; // top-aligned with esp_y + esp_h below
  esp_y = wall + inner_h - esp_h; // top-aligned to same interior top as TFT

  difference() {
    union() {
      // outer shell
      linear_extrude(height = floor_t + inner_d)
        rounded_rect(outer_w, outer_h, corner_r);
    }
    // hollow out interior, leave floor
    translate([wall, wall, floor_t])
      linear_extrude(height = inner_d + 1)
        rounded_rect(inner_w, inner_h, corner_r - wall > 0 ? corner_r - wall : 1);

    // USB-C access cutout — top wall, aligned to the ESP32 board's top edge
    // (adjust which wall this is on if your board's USB connector is elsewhere)
    translate([esp_x + esp_w/2 - 5, outer_h - wall - 1, floor_t + esp_standoff_h + 1])
      cube([10, wall + 2, 6]);
  }

  // TFT board standoffs
  translate([tft_x + tft_mount_inset, tft_y + tft_mount_inset, floor_t])
    standoff(tft_hole_dia, tft_standoff_h);
  translate([tft_x + tft_w - tft_mount_inset, tft_y + tft_mount_inset, floor_t])
    standoff(tft_hole_dia, tft_standoff_h);
  translate([tft_x + tft_mount_inset, tft_y + tft_h - tft_mount_inset, floor_t])
    standoff(tft_hole_dia, tft_standoff_h);
  translate([tft_x + tft_w - tft_mount_inset, tft_y + tft_h - tft_mount_inset, floor_t])
    standoff(tft_hole_dia, tft_standoff_h);

  // ESP32-S3 board standoffs
  translate([esp_x + esp_mount_inset, esp_y + esp_mount_inset, floor_t])
    standoff(esp_hole_dia, esp_standoff_h);
  translate([esp_x + esp_w - esp_mount_inset, esp_y + esp_mount_inset, floor_t])
    standoff(esp_hole_dia, esp_standoff_h);
  translate([esp_x + esp_mount_inset, esp_y + esp_h - esp_mount_inset, floor_t])
    standoff(esp_hole_dia, esp_standoff_h);
  translate([esp_x + esp_w - esp_mount_inset, esp_y + esp_h - esp_mount_inset, floor_t])
    standoff(esp_hole_dia, esp_standoff_h);

  // corner screw bosses (joins to lid)
  corner_bosses(outer_w, outer_h, floor_t + inner_d - 2);
}

// ============================================================
// LID (top shell)
// ============================================================
module case_lid() {
  esp_x = wall;
  tft_x = wall + esp_w + column_gap;
  tft_y = wall + button_zone_h;
  esp_y = wall + inner_h - esp_h;

  difference() {
    union() {
      linear_extrude(height = lid_t)
        rounded_rect(outer_w, outer_h, corner_r);
      // thin alignment rim (frame, not a plate) that nests inside the base walls —
      // only the outer lip_frame_w band is solid, the interior stays open so it
      // can never block the screen window, button holes, or wire routing space.
      translate([wall + gap, wall + gap, -2])
        linear_extrude(height = 2)
          difference() {
            rounded_rect(inner_w - 2*gap, inner_h - 2*gap, max(corner_r - wall, 1));
            translate([lip_frame_w, lip_frame_w, 0])
              rounded_rect(inner_w - 2*gap - 2*lip_frame_w, inner_h - 2*gap - 2*lip_frame_w,
                            max(corner_r - wall - lip_frame_w, 0.5));
          }
    }

    // screen window, aligned over the TFT board's active area
    // (cut depth -3 to lid_t+2 so it fully clears the rim's z=-2 to 0 span too)
    translate([tft_x + tft_active_off_x, tft_y + tft_active_off_y, -3])
      linear_extrude(height = lid_t + 5)
        square([tft_active_w, tft_active_h]);

    // button holes — centered under the TFT column's screen, in its button strip
    btn_y = button_zone_h / 2 + wall/2;
    btn_center_x = tft_x + tft_w/2;
    for (i = [-1, 0, 1])
      translate([btn_center_x + i * btn_spacing, btn_y, -3])
        cylinder(d = btn_hole_dia, h = lid_t + 5, $fn = 32);

    // corner screw holes (through-holes matching base bosses)
    positions = [[6, 6], [outer_w - 6, 6], [6, outer_h - 6], [outer_w - 6, outer_h - 6]];
    for (p = positions)
      translate([p[0], p[1], -3])
        cylinder(d = 2.2, h = lid_t + 5, $fn = 16);
  }
}

// ============================================================
// Render selection
// ============================================================
// SKIP_MAIN_RENDER lets other .scad files `include` this one (to reuse
// its variables/modules) without it auto-rendering its own output.
if (is_undef(SKIP_MAIN_RENDER) || !SKIP_MAIN_RENDER) {
  if (PART == "base") {
    case_base();
  } else if (PART == "lid") {
    case_lid();
  } else {
    case_base();
    translate([outer_w + 15, 0, 0]) case_lid();
  }
}
