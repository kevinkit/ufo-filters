/*
 * Copyright (C) 2011-2013 Karlsruhe Institute of Technology
 *
 * This file is part of Ufo.
 *
 * This library is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

constant sampler_t volumeSampler = CLK_NORMALIZED_COORDS_FALSE |
                                   CLK_ADDRESS_CLAMP |
                                   CLK_FILTER_LINEAR;

#define PI 3.1415926535897932384626433832795028841971693993751058209749445923078164062f

kernel void
backproject_nearest (global float *sinogram,
                     global float *slice,
                     constant float *sin_lut,
                     constant float *cos_lut,
                     const unsigned int offset,
                     const unsigned n_projections,
                     const float axis_pos)
{
    const int idx = get_global_id(0);
    const int idy = get_global_id(1);
    const int width = get_global_size(0);
    const float bx = idx - axis_pos;
    const float by = idy - axis_pos;
    float sum = 0.0;

    for(int proj = 0; proj < n_projections; proj++) {
        float h = axis_pos + bx * cos_lut[offset + proj] + by * sin_lut[offset + proj];
        sum += sinogram[(int)(proj * width + h)];
    }

    slice[idy * width + idx] = sum * 4.0 * PI;
}

kernel void
backproject_tex (read_only image2d_t sinogram,
                 global float *slice,
                 constant float *sin_lut,
                 constant float *cos_lut,
                 const unsigned int offset,
                 const unsigned int n_projections,
                 const float axis_pos)
{
    const int idx = get_global_id(0);
    const int idy = get_global_id(1);
    const int width = get_global_size(0);
    const float bx = idx - axis_pos;
    const float by = idy - axis_pos;
    float sum = 0.0f;

    for(int proj = 0; proj < n_projections; proj++) {
        /* mad() instructions have a performance impact of about 1% on GTX 580 */
        /* float h = mad (by, sin_lut[proj], mad(bx, cos_lut[proj], axis_pos)); */

        float h = by * sin_lut[offset + proj] + bx * cos_lut[offset + proj] + axis_pos;
        float val = read_imagef (sinogram, volumeSampler, (float2)(h, proj + 0.5f)).x;
        sum += (isnan (val) ? 0.0 : val);
    }

    slice[idy * width + idx] = sum * 4.0 * PI;
}

