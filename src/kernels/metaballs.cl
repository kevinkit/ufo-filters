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

kernel void
draw_metaballs (global float *output,
                global float2 *positions,
                constant float *sizes,
                uint num_balls)
{
    const int idx = get_global_id(0);
    const int idy = get_global_id(1);
    const float r = 1.0f;

    float sum = 0.0f;
    for (int i = 0; i < num_balls; i++) {
        float x = (positions[i].x - idx);
        float y = (positions[i].y - idy);
        sum += sizes[i] / sqrt(x*x + y*y);
    }
    if ((sum > (r - 0.01f)) && (sum < (r + 0.01f)))
        output[idy * get_global_size(0) + idx] = 1.0f;
    else
        output[idy * get_global_size(0) + idx] = 0.0f;
}

