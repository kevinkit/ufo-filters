__kernel void found_cand(__global float* input,__global float* positions,__global float* values, __global unsigned* counter)
{
    int id = get_global_id(0);
    int old;

    if(input[id] > 0.0)
    {
        old  = atomic_inc(&counter[0]);
        positions[old] = id;
        values[old] = input[id]; //I do not know if this is necessary

    }


}
