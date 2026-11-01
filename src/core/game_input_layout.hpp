#pragma once

#include <d3d11_1.h>

namespace sp::core {

struct Game_input_layout {
   UINT16 layout_index;
   bool compressed_position;
   bool compressed_texcoords;
   bool has_vertex_weights;
   // VR (reading a model's positions back): where stream 0's position is in a vertex,
   // and how it's stored (0 unknown, 1 three floats, 2 four shorts, compressed).
   UINT16 position_offset = 0;
   UINT8 position_kind = 0;
};

}
