#pragma once

// =======================================================
// UNIT SIDE CONFIGURATION
// =======================================================
//
// There are two LCD units in the system. Both work on the same program
// but with different configuration. You have to choose 
// which side of the table is currently being programmed.
//
// 'A' = side near electronics
// 'B' = side far from electronics
//
// This affects:
// - which score is shown on OLED1 / OLED2
// - which character is used (Rayman / Globox)
// - animation mood logic (win / lose)

constexpr char UNIT_SIDE = 'B';   // SET SIDE A or B.

