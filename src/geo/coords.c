#include "mesh/geo/coords.h"

bool mesh_geo_coords_valid(int32_t latitude_i, int32_t longitude_i) {
    return latitude_i <= MESH_GEO_LATITUDE_I_MAX && latitude_i >= -MESH_GEO_LATITUDE_I_MAX &&
           longitude_i <= MESH_GEO_LONGITUDE_I_MAX && longitude_i >= -MESH_GEO_LONGITUDE_I_MAX;
}
