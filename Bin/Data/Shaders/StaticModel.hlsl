//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

struct PSInput
{
    float4 position : SV_POSITION;
    float2 tex : TEXCOORD;
};

// #DXR Extra: Perspective Camera
cbuffer CameraParams : register(b0)
{
    float4x4 view;
    float4x4 projection;
}

PSInput VSMain(float3 position : POSITION, float2 tex : TEXCOORD, float3 normal : NORMAL, float3 biNormal : BINORMAL, float3 tangent : TANGENT )
{
    PSInput result;
    // #DXR Extra: Perspective Camera
    float4 pos = float4(position, 1);
    pos = mul(view, pos);
    pos = mul(projection, pos);
    result.position = pos;
    result.tex = tex;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return float4(1,1,0,1);
}
