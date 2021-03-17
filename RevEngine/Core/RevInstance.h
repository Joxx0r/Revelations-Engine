﻿#pragma once
#include <DirectXMath.h>

#include "RevModelManager.h"

class RevInstance
{
public:
    RevInstance() {};

    void Initialize(RevModelManager* manager,  ID3D12Device5* device, int modelType, DirectX::XMMATRIX transform,  Microsoft::WRL::ComPtr<ID3D12Resource> resource);
    void DrawInstance(ID3D12GraphicsCommandList4* list);

    DirectX::XMMATRIX m_transform;

    ID3D12Device5* m_device = nullptr;
    RevModelManager* m_modelManager = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource = nullptr;
    int m_modelHandle;
    int m_instanceHandle;
};

