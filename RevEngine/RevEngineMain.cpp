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
// ReSharper disable All
#include "stdafx.h"
#include "Win32Application.h"
#include "RevEngineMain.h"
#include "DXRHelper.h"
#include "BottomLevelASGenerator.h"
#include "RaytracingPipelineGenerator.h"
#include "RootSignatureGenerator.h"
#include "Windowsx.h"
#include "Core/RevInstanceManager.h"
#include "Core/RevModelManager.h"
#include "Core/RevModelTypes.h"
#include "Core/RevScene.h"
#include "Core/RevShaderManager.h"

RevEngineMain* RevEngineMain::s_instance = nullptr;

RevEngineMain::RevEngineMain(const RevEngineInitializationData& data)
	:m_windowData(data.m_windowData), m_viewport(0.0f, 0.0f, static_cast<float>(data.m_windowData.m_width), static_cast<float>(data.m_windowData.m_height)),  m_scissorRect(0, 0, static_cast<LONG>(data.m_windowData.m_width), static_cast<LONG>(data.m_windowData.m_height)), m_rtvDescriptorSize(0)
{
	m_initData = data;
	m_isRasterizationActive = data.m_rasterDefault;
	WCHAR assetsPath[512];
	GetAssetsPath(assetsPath, _countof(assetsPath));
	m_assetsPath = assetsPath;
	m_camera.Initialize(m_windowData.GetAspectRatio());
	m_modelManager = new RevModelManager();
	m_shaderManager = new RevShaderManager();
}
RevEngineMain* RevEngineMain::Construct(const RevEngineInitializationData& data)
{
	if(s_instance)
	{
		return s_instance;
	}
	s_instance = new RevEngineMain(data);
	return s_instance;
}

void RevEngineMain::Destroy()
{
	if(!s_instance)
	{
		return;
	}
	delete(s_instance);
}

RevEngineMain* RevEngineMain::Get()
{
	return s_instance;
}

void RevEngineMain::OnInit()
{	
	LoadPipeline();
	LoadAssets();
	CheckRaytracingSupport();
	
	CreateAccelerationStructures();
	ThrowIfFailed(m_commandList->Close());
	CreateRaytracingPipeline();
	CreatePerInstanceConstantBuffers();
	CreateRaytracingOutputBuffer();
	CreateCameraBuffer();
	CreateShaderResourceHeap();
	CreateShaderBindingTable();
	m_camera.Update(0.0f, m_input);
	UpdateCameraBuffer();
}

// Load the rendering pipeline dependencies.
void RevEngineMain::LoadPipeline()
{
	UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the
	// active device.
	{
		ComPtr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();

			// Enable additional debug layers.
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}
	}
#endif

	ComPtr<IDXGIFactory4> factory;
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));
	ComPtr<IDXGIAdapter1> hardwareAdapter;
	GetHardwareAdapter(factory.Get(), &hardwareAdapter);

	ThrowIfFailed(D3D12CreateDevice(hardwareAdapter.Get(),
	                                D3D_FEATURE_LEVEL_12_1,
	                                IID_PPV_ARGS(&m_device)));
	// Describe and create the command queue.
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	ThrowIfFailed(
		m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));

	// Describe and create the swap chain.
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = FrameCount;
	swapChainDesc.Width = m_windowData.m_width;
	swapChainDesc.Height = m_windowData.m_height;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;

	ComPtr<IDXGISwapChain1> swapChain;
	ThrowIfFailed(factory->CreateSwapChainForHwnd(
		m_commandQueue.Get(), // Swap chain needs the queue so that it can force a
		// flush on it.
		Win32Application::GetHwnd(), &swapChainDesc, nullptr, nullptr,
		&swapChain));

	// This sample does not support fullscreen transitions.
	ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(),
	                                             DXGI_MWA_NO_ALT_ENTER));

	ThrowIfFailed(swapChain.As(&m_swapChain));
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// Create descriptor heaps.
	{
		// Describe and create a render target view (RTV) descriptor heap.
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
		rtvHeapDesc.NumDescriptors = FrameCount;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		ThrowIfFailed(
			m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

		m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
			D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}

	// Create frame resources.
	{
		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
			m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

		// Create a RTV for each frame.
		for (UINT n = 0; n < FrameCount; n++)
		{
			ThrowIfFailed(
				m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
			m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr,
			                                 rtvHandle);
			rtvHandle.Offset(1, m_rtvDescriptorSize);
		}
	}

	ThrowIfFailed(m_device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));
	CreateDepthBuffer();
}

// Load the sample assets.
void RevEngineMain::LoadAssets()
{
	// Create an empty root signature.
	{
		CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
		rootSignatureDesc.Init(
			0, nullptr, 0, nullptr,
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// #DXR Extra: Perspective Camera
		// The root signature describes which data is accessed by the shader. The camera matrices are held
		// in a constant buffer, itself referenced the heap. To do this we reference a range in the heap,
		// and use that range as the sole parameter of the shader. The camera buffer is associated in the
		// index 0, making it accessible in the shader in the b0 register.
		CD3DX12_ROOT_PARAMETER constantParameter;
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
		constantParameter.InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);

		rootSignatureDesc.Init(1, &constantParameter, 0, nullptr,
                               D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
		
		ComPtr<ID3DBlob> signature;
		ComPtr<ID3DBlob> error;
		ThrowIfFailed(D3D12SerializeRootSignature(
			&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
		ThrowIfFailed(m_device->CreateRootSignature(
			0, signature->GetBufferPointer(), signature->GetBufferSize(),
			IID_PPV_ARGS(&m_rootSignature)));
		
	}

	

	// Create the pipeline state, which includes compiling and loading shaders.
	{

		const std::wstring shader_path = L"Data//Shaders//Shaders.hlsl";
		RevShaderRasterizer* rasterizer = m_shaderManager->GetShaderRasterizer(shader_path);
		// Define the vertex input layout.
		D3D12_INPUT_ELEMENT_DESC inputElementDesc[] = {
			{
				"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
				D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
			},
			{
				"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
				D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
			}
		};

		// Describe and create the graphics pipeline state object (PSO).
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.InputLayout = {inputElementDesc, _countof(inputElementDesc)};
		psoDesc.pRootSignature = m_rootSignature.Get();
		psoDesc.VS = CD3DX12_SHADER_BYTECODE(rasterizer->m_vertexShader.Get());
		psoDesc.PS = CD3DX12_SHADER_BYTECODE(rasterizer->m_pixelShader.Get());
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);;
		psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.SampleDesc.Count = 1;
		ThrowIfFailed(m_device->CreateGraphicsPipelineState(
			&psoDesc, IID_PPV_ARGS(&m_pipelineState)));
	}

	// Create the command list.
	ThrowIfFailed(m_device->CreateCommandList(
		0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(),
		m_pipelineState.Get(), IID_PPV_ARGS(&m_commandList)));

	// Create synchronization objects and wait until assets have been uploaded to
	// the GPU.
	{
		ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
		                                    IID_PPV_ARGS(&m_fence)));
		m_fenceValue = 1;

		// Create an event handle to use for frame synchronization.
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (m_fenceEvent == nullptr)
		{
			ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}

		// Wait for the command list to execute; we are reusing the same command
		// list in our main loop but for now, we just want to wait for setup to
		// complete before continuing.
		WaitForPreviousFrame();
	}
	m_shaderManager->Initialize();
}


// Update frame-based values.
void RevEngineMain::OnUpdate(float delta)
{
	UpdateInput(delta);
	UpdateCameraBuffer();
}

// Render the scene.
void RevEngineMain::OnRender()
{
	// Record all the commands we need to render the scene into the command list.
	PopulateCommandList();

	// Execute the command list.
	ID3D12CommandList* ppCommandLists[] = {m_commandList.Get()};
	m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

	// Present the frame.
	ThrowIfFailed(m_swapChain->Present(1, 0));

	WaitForPreviousFrame();
}

void RevEngineMain::OnDestroy()
{
	// Ensure that the GPU is no longer referencing resources that are about to be
	// cleaned up by the destructor.
	WaitForPreviousFrame();

	CloseHandle(m_fenceEvent);
}

void RevEngineMain::PopulateCommandList() const
{
	// Command list allocators can only be reset when the associated
	// command lists have finished execution on the GPU; apps should use
	// fences to determine GPU execution progress.
	ThrowIfFailed(m_commandAllocator->Reset());

	// However, when ExecuteCommandList() is called on a particular command
	// list, that command list can then be reset at any time and must be before
	// re-recording.
	ThrowIfFailed(
		m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()));

	// Set necessary state.
	m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
	m_commandList->RSSetViewports(1, &m_viewport);
	m_commandList->RSSetScissorRects(1, &m_scissorRect);

	// Indicate that the back buffer will be used as a render target.
	m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		                               m_renderTargets[m_frameIndex].Get(),
		                               D3D12_RESOURCE_STATE_PRESENT,
		                               D3D12_RESOURCE_STATE_RENDER_TARGET));

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
		m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex,
		m_rtvDescriptorSize);
	// #DXR Extra: Depth Buffering
	// Bind the depth buffer as a render target
	CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
	m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

	// Record commands.
	if (m_isRasterizationActive)
	{
		// #DXR Extra: Depth Buffering
		m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		std::vector< ID3D12DescriptorHeap* > heaps = { m_constHeap.Get() };
		m_commandList->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
		// set the root descriptor table 0 to the constant buffer descriptor heap
		m_commandList->SetGraphicsRootDescriptorTable(
          0, m_constHeap->GetGPUDescriptorHandleForHeapStart());

		
		const float clearColor[] = {0.0f, 0.2f, 0.4f, 1.0f};
		m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
		RevDrawData drawData = {};
		drawData.m_cameraCB = m_cameraBuffer.Get();
		m_scene->DrawScene(drawData);
	}
	else
	{
		// #DXR
		// Bind the descriptor heap giving access to the top-level acceleration
		// structure, as well as the raytracing output
		std::vector<ID3D12DescriptorHeap*> heaps = {m_srvUavHeap.Get()};
		m_commandList->SetDescriptorHeaps(static_cast<UINT>(heaps.size()),
		                                  heaps.data());

		// On the last frame, the raytracing output was used as a copy source, to
		// copy its contents into the render target. Now we need to transition it to
		// a UAV so that the shaders can write in it.
		CD3DX12_RESOURCE_BARRIER transition = CD3DX12_RESOURCE_BARRIER::Transition(
			m_outputResource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		m_commandList->ResourceBarrier(1, &transition);

		// Setup the raytracing task
		D3D12_DISPATCH_RAYS_DESC desc = {};
		// The layout of the SBT is as follows: ray generation shader, miss
		// shaders, hit groups. As described in the CreateShaderBindingTable method,
		// all SBT entries of a given type have the same size to allow a fixed
		// stride.

		// The ray generation shaders are always at the beginning of the SBT.
		const uint32_t rayGenerationSectionSizeInBytes =
			m_sbtHelper.GetRayGenSectionSize();
		desc.RayGenerationShaderRecord.StartAddress =
			m_sbtStorage->GetGPUVirtualAddress();
		desc.RayGenerationShaderRecord.SizeInBytes =
			rayGenerationSectionSizeInBytes;

		// The miss shaders are in the second SBT section, right after the ray
		// generation shader. We have one miss shader for the camera rays and one
		// for the shadow rays, so this section has a size of 2*m_sbtEntrySize. We
		// also indicate the stride between the two miss shaders, which is the size
		// of a SBT entry
		const uint32_t missSectionSizeInBytes = m_sbtHelper.GetMissSectionSize();
		desc.MissShaderTable.StartAddress =
			m_sbtStorage->GetGPUVirtualAddress() + rayGenerationSectionSizeInBytes;
		desc.MissShaderTable.SizeInBytes = missSectionSizeInBytes;
		desc.MissShaderTable.StrideInBytes = m_sbtHelper.GetMissEntrySize();

		// The hit groups section start after the miss shaders. In this sample we
		// have one 1 hit group for the triangle
		const uint32_t hitGroupsSectionSize = m_sbtHelper.GetHitGroupSectionSize();
		desc.HitGroupTable.StartAddress = m_sbtStorage->GetGPUVirtualAddress() +
			rayGenerationSectionSizeInBytes +
			missSectionSizeInBytes;
		desc.HitGroupTable.SizeInBytes = hitGroupsSectionSize;
		desc.HitGroupTable.StrideInBytes = m_sbtHelper.GetHitGroupEntrySize();

		// Dimensions of the image to render, identical to a kernel launch dimension
		desc.Width = GetWidth();
		desc.Height = GetHeight();
		desc.Depth = 1;

		// Bind the raytracing pipeline
		m_commandList->SetPipelineState1(m_rtStateObject.Get());
		// Dispatch the rays and write to the raytracing output
		m_commandList->DispatchRays(&desc);

		// The raytracing output needs to be copied to the actual render target used
		// for display. For this, we need to transition the raytracing output from a
		// UAV to a copy source, and the render target buffer to a copy destination.
		// We can then do the actual copy, before transitioning the render target
		// buffer into a render target, that will be then used to display the image
		transition = CD3DX12_RESOURCE_BARRIER::Transition(
			m_outputResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		m_commandList->ResourceBarrier(1, &transition);
		transition = CD3DX12_RESOURCE_BARRIER::Transition(
			m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_COPY_DEST);
		m_commandList->ResourceBarrier(1, &transition);

		m_commandList->CopyResource(m_renderTargets[m_frameIndex].Get(),
		                            m_outputResource.Get());

		transition = CD3DX12_RESOURCE_BARRIER::Transition(
			m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_RENDER_TARGET);
		m_commandList->ResourceBarrier(1, &transition);
	}

	// Indicate that the back buffer will now be used to present.
	m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
		                               m_renderTargets[m_frameIndex].Get(),
		                               D3D12_RESOURCE_STATE_RENDER_TARGET,
		                               D3D12_RESOURCE_STATE_PRESENT));

	ThrowIfFailed(m_commandList->Close());
}

void RevEngineMain::WaitForPreviousFrame()
{
	// WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
	// This is code implemented as such for simplicity. The
	// D3D12HelloFrameBuffering sample illustrates how to use fences for efficient
	// resource usage and to maximize GPU utilization.

	// Signal and increment the fence value.
	const UINT64 fence = m_fenceValue;
	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fence));
	m_fenceValue++;

	// Wait until the previous frame is finished.
	if (m_fence->GetCompletedValue() < fence)
	{
		ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}

	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}
void RevEngineMain::FlushCommandQueue()
{
	m_fenceValue++;

	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValue));

	if (m_fence->GetCompletedValue() < m_fenceValue)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
		ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValue, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}
}

void RevEngineMain::CheckRaytracingSupport() const
{
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
	ThrowIfFailed(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
	                                            &options5, sizeof(options5)));
	if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
		throw std::runtime_error("Raytracing not supported on device");
}

//-----------------------------------------------------------------------------
//
//
void RevEngineMain::OnKeyUp(const UINT8 key)
{
	if (key == VK_SPACE)
	{
		m_isRasterizationActive = !m_isRasterizationActive;
	}
}

void RevEngineMain::OnButtonDown(UINT32 lParam)
{
	m_input.m_startX = -GET_X_LPARAM(lParam);
	m_input.m_startY = -GET_Y_LPARAM(lParam);
}

void RevEngineMain::OnMouseMove(UINT8 wParam, UINT32 lParam)
{
	m_input.m_leftButton = wParam & MK_LBUTTON;
	m_input.m_middleButton = wParam & MK_MBUTTON;
	m_input.m_rightButton = wParam & MK_RBUTTON;
	m_input.m_x = -GET_X_LPARAM(lParam);
	m_input.m_y = -GET_Y_LPARAM(lParam);
	if(m_input.m_leftButton)
	{
		const int deltaX = m_input.m_startX - m_input.m_x;
		const int deltaY = m_input.m_y - m_input.m_startY;
		m_camera.OnMoveDelta(static_cast<float>(deltaX), static_cast<float>(deltaY));
		m_input.m_startX = m_input.m_x;
		m_input.m_startY = m_input.m_y;
	}
}

void RevEngineMain::UpdateInput(float delta)
{
	m_input.m_ctrl = GetAsyncKeyState(VK_CONTROL);
	m_input.m_shift = GetAsyncKeyState(VK_SHIFT);
	m_input.m_alt = GetAsyncKeyState(VK_MENU);
	m_input.m_ctrl = GetAsyncKeyState(VK_CONTROL);
	m_input.m_shift = GetAsyncKeyState(VK_SHIFT);
	m_input.m_alt = GetAsyncKeyState(VK_MENU);
	m_input.m_left = GetAsyncKeyState(65);
	m_input.m_right = GetAsyncKeyState(68);
	m_input.m_up = GetAsyncKeyState(VK_UP);
	m_input.m_down = GetAsyncKeyState(VK_DOWN);
	m_input.m_up = GetAsyncKeyState(VK_UP);
	m_input.m_down = GetAsyncKeyState(VK_DOWN);
	m_input.m_forward = GetAsyncKeyState(87);
	m_input.m_back = GetAsyncKeyState(83);
	m_camera.Update(delta, m_input);
}
void RevEngineMain::CreateTopLevelAS()
{
	// Gather all the instances into the builder helper
	m_scene->m_instanceManager->AddAllInstancesToSBT(&m_topLevelASGenerator);

	// As for the bottom-level AS, the building the AS requires some scratch space
	// to store temporary data in addition to the actual AS. In the case of the
	// top-level AS, the instance descriptors also need to be stored in GPU
	// memory. This call outputs the memory requirements for each (scratch,
	// results, instance descriptors) so that the application can allocate the
	// corresponding memory
	UINT64 scratchSize, resultSize, instanceDescSize;

	m_topLevelASGenerator.ComputeASBufferSizes(m_device.Get(), true, &scratchSize,
	                                           &resultSize, &instanceDescSize);

	// Create the scratch and result buffers. Since the build is all done on GPU,
	// those can be allocated on the default heap
	m_topLevelASBuffers.pScratch = nv_helpers_dx12::CreateBuffer(
		m_device.Get(), scratchSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nv_helpers_dx12::kDefaultHeapProps);
	m_topLevelASBuffers.pResult = nv_helpers_dx12::CreateBuffer(
		m_device.Get(), resultSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		nv_helpers_dx12::kDefaultHeapProps);

	// The buffer describing the instances: ID, shader binding information,
	// matrices ... Those will be copied into the buffer by the helper through
	// mapping, so the buffer has to be allocated on the upload heap.
	m_topLevelASBuffers.pInstanceDesc = nv_helpers_dx12::CreateBuffer(
		m_device.Get(), instanceDescSize, D3D12_RESOURCE_FLAG_NONE,
		D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);

	// After all the buffers are allocated, or if only an update is required, we
	// can build the acceleration structure. Note that in the case of the update
	// we also pass the existing AS as the 'previous' AS, so that it can be
	// refitted in place.
	m_topLevelASGenerator.Generate(m_commandList.Get(),
	                               m_topLevelASBuffers.pScratch.Get(),
	                               m_topLevelASBuffers.pResult.Get(),
	                               m_topLevelASBuffers.pInstanceDesc.Get());
}

void RevEngineMain::CreateAccelerationStructures()
{
	m_scene = new RevScene();
	m_scene->Initialize();
	
	RevModelManager::GenerateAccelerationBuffersAllModels();
	
	// Build the bottom AS from the Triangle vertex buffer
	CreateTopLevelAS();

	// Flush the command list and wait for it to finish
	m_commandList->Close();
	ID3D12CommandList* ppCommandLists[] = {m_commandList.Get()};
	m_commandQueue->ExecuteCommandLists(1, ppCommandLists);
	m_fenceValue++;
	m_commandQueue->Signal(m_fence.Get(), m_fenceValue);

	m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
	WaitForSingleObject(m_fenceEvent, INFINITE);

	// Once the command list is finished executing, reset it to be reused for
	// rendering
	ThrowIfFailed(
		m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()));
}

//-----------------------------------------------------------------------------
// The ray generation shader needs to access 2 resources: the raytracing output
// and the top-level acceleration structure
//

ComPtr<ID3D12RootSignature> RevEngineMain::CreateRayGenSignature() const
{
	nv_helpers_dx12::RootSignatureGenerator rsc;
	rsc.AddHeapRangesParameter(
      {{0 /*u0*/, 1 /*1 descriptor */, 0 /*use the implicit register space 0*/,
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV /* UAV representing the output buffer*/,
        0 /*heap slot where the UAV is defined*/},
       {0 /*t0*/, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV /*Top-level acceleration structure*/, 1},
       {0 /*b0*/, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_CBV /*Camera parameters*/, 2}});
	return rsc.Generate(m_device.Get(), true);
}

//-----------------------------------------------------------------------------
// The hit shader communicates only through the ray payload, and therefore does
// not require any resources
//
ComPtr<id3d12rootsignature> RevEngineMain::CreateHitSignature() const
{
	nv_helpers_dx12::RootSignatureGenerator rsc;
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 0);
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV);
	return rsc.Generate(m_device.Get(), true);
}

//-----------------------------------------------------------------------------
// The miss shader communicates only through the ray payload, and therefore
// does not require any resources
//
ComPtr<id3d12rootsignature> RevEngineMain::CreateMissSignature() const
{
	nv_helpers_dx12::RootSignatureGenerator rsc;
	return rsc.Generate(m_device.Get(), true);
}

//-----------------------------------------------------------------------------
//
// The raytracing pipeline binds the shader code, root signatures and pipeline
// characteristics in a single structure used by DXR to invoke the shaders and
// manage temporary memory during raytracing
//
//
void RevEngineMain::CreateRaytracingPipeline()
{
	nv_helpers_dx12::RayTracingPipelineGenerator pipeline(m_device.Get());

	pipeline.AddLibrary(RevShaderManager::GetShaderLibrary(L"Data//Shaders//RayGen.hlsl")->m_blob, {L"RayGen"});
	pipeline.AddLibrary(RevShaderManager::GetShaderLibrary(L"Data/Shaders//Miss.hlsl")->m_blob, {L"Miss"});
	pipeline.AddLibrary(RevShaderManager::GetShaderLibrary(L"Data//Shaders//Hit.hlsl")->m_blob, {L"ClosestHit", L"PlaneClosestHit"});

	m_rayGenSignature = CreateRayGenSignature();
	m_missSignature = CreateMissSignature();
	m_hitSignature = CreateHitSignature();

	pipeline.AddHitGroup(L"HitGroup", L"ClosestHit");
	pipeline.AddHitGroup(L"PlaneHitGroup", L"PlaneClosestHit");
	pipeline.AddRootSignatureAssociation(m_rayGenSignature.Get(), {L"RayGen"});
	pipeline.AddRootSignatureAssociation(m_missSignature.Get(), {L"Miss"});
	pipeline.AddRootSignatureAssociation(m_hitSignature.Get(), {L"HitGroup", L"PlaneHitGroup"});

	pipeline.SetMaxPayloadSize(4 * sizeof(float)); // RGB + distance

	pipeline.SetMaxAttributeSize(2 * sizeof(float)); // barycentric coordinates

	pipeline.SetMaxRecursionDepth(1);

	m_rtStateObject = pipeline.Generate();
	ThrowIfFailed(
		m_rtStateObject->QueryInterface(IID_PPV_ARGS(&m_rtStateObjectProps)));
}

//-----------------------------------------------------------------------------
//
// Allocate the buffer holding the raytracing output, with the same size as the
// output image
//
void RevEngineMain::CreateRaytracingOutputBuffer()
{
	D3D12_RESOURCE_DESC resDesc = {};
	resDesc.DepthOrArraySize = 1;
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

	resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	resDesc.Width = GetWidth();
	resDesc.Height = GetHeight();
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resDesc.MipLevels = 1;
	resDesc.SampleDesc.Count = 1;
	ThrowIfFailed(m_device->CreateCommittedResource(
		&nv_helpers_dx12::kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
		D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
		IID_PPV_ARGS(&m_outputResource)));
}

//-----------------------------------------------------------------------------
//
// Create the main heap used by the shaders, which will give access to the
// raytracing output and the top-level acceleration structure
//
void RevEngineMain::CreateShaderResourceHeap()
{
	m_srvUavHeap = nv_helpers_dx12::CreateDescriptorHeap(
		m_device.Get(), 3, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);

	// Get a handle to the heap memory on the CPU side, to be able to write the
	// descriptors directly
	D3D12_CPU_DESCRIPTOR_HANDLE srvHandle =
		m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();

	// Create the UAV. Based on the root signature we created it is the first
	// entry. The Create*View methods write the view information directly into
	// srvHandle
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	m_device->CreateUnorderedAccessView(m_outputResource.Get(), nullptr, &uavDesc,
	                                    srvHandle);

	// Add the Top Level AS SRV right after the raytracing output buffer
	srvHandle.ptr += m_device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.RaytracingAccelerationStructure.Location =
		m_topLevelASBuffers.pResult->GetGPUVirtualAddress();
	// Write the acceleration structure view in the heap
	m_device->CreateShaderResourceView(nullptr, &srvDesc, srvHandle);
	// #DXR Extra: Perspective Camera
	// Add the constant buffer for the camera after the TLAS
	srvHandle.ptr +=
      m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// Describe and create a constant buffer view for the camera
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	cbvDesc.BufferLocation = m_cameraBuffer->GetGPUVirtualAddress();
	cbvDesc.SizeInBytes = m_cameraBufferSize;
	m_device->CreateConstantBufferView(&cbvDesc, srvHandle);
	
}

//-----------------------------------------------------------------------------
//
// The Shader Binding Table (SBT) is the cornerstone of the raytracing setup:
// this is where the shader resources are bound to the shaders, in a way that
// can be interpreted by the raytracer on GPU. In terms of layout, the SBT
// contains a series of shader IDs with their resource pointers. The SBT
// contains the ray generation shader, the miss shaders, then the hit groups.
// Using the helper class, those can be specified in arbitrary order.
//
void RevEngineMain::CreateShaderBindingTable()
{
	// The SBT helper class collects calls to Add*Program.  If called several
	// times, the helper must be emptied before re-adding shaders.
	m_sbtHelper.Reset();

	// The pointer to the beginning of the heap is the only parameter required by
	// shaders without root parameters
	const D3D12_GPU_DESCRIPTOR_HANDLE srvUavHeapHandle =
		m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();

	// The helper treats both root parameter pointers and heap pointers as void*,
	// while DX12 uses the
	// D3D12_GPU_DESCRIPTOR_HANDLE to define heap pointers. The pointer in this
	// struct is a UINT64, which then has to be reinterpreted as a pointer.
	UINT64* heapPointer = reinterpret_cast<UINT64*>(srvUavHeapHandle.ptr);

	// The ray generation only uses heap data
	m_sbtHelper.AddRayGenerationProgram(L"RayGen", {heapPointer});

	// The miss and hit shaders do not access any external resources: instead they
	// communicate their results through the ray payload
	m_sbtHelper.AddMissProgram(L"Miss", {});

	// #DXR Extra: Per-Instance Data
	// Adding the triangle hit shader and constant buffer data


	// #DXR Extra: Per-Instance Data
	// We have 3 triangles, each of which needs to access its own constant buffer
	// as a root parameter in its primary hit shader. The shadow hit only sets a
	// boolean visibility in the payload, and does not require external data
	for (int i = 0; i < 3; ++i)
	{
		m_sbtHelper.AddHitGroup(
            L"HitGroup",
            {reinterpret_cast<void*>(m_perInstanceConstantBuffers[i]->GetGPUVirtualAddress())
            });
	}

	// #DXR Extra: Per-Instance Data
	// Adding the plane
	m_sbtHelper.AddHitGroup(L"PlaneHitGroup", {});

	// #DXR Extra: Per-Instance Data
	// Adding the plane
	//m_sbtHelper.AddHitGroup(L"PlaneHitGroup", {});
	

	// Compute the size of the SBT given the number of shaders and their
	// parameters
	const uint32_t sbtSize = m_sbtHelper.ComputeSBTSize();

	// Create the SBT on the upload heap. This is required as the helper will use
	// mapping to write the SBT contents. After the SBT compilation it could be
	// copied to the default heap for performance.
	m_sbtStorage = nv_helpers_dx12::CreateBuffer(
		m_device.Get(), sbtSize, D3D12_RESOURCE_FLAG_NONE,
		D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
	if (!m_sbtStorage)
	{
		throw std::logic_error("Could not allocate the shader binding table");
	}
	// Compile the SBT from the shader and parameters info
	m_sbtHelper.Generate(m_sbtStorage.Get(), m_rtStateObjectProps.Get());
}

//----------------------------------------------------------------------------------
//
// The camera buffer is a constant buffer that stores the transform matrices of
// the camera, for use by both the rasterization and raytracing. This method
// allocates the buffer where the matrices will be copied. For the sake of code
// clarity, it also creates a heap containing only this buffer, to use in the
// rasterization path.
//
// #DXR Extra: Perspective Camera
void RevEngineMain::CreateCameraBuffer() {
	uint32_t nbMatrix = 4; // view, perspective, viewInv, perspectiveInv
	m_cameraBufferSize = nbMatrix * sizeof(XMMATRIX);

	// Create the constant buffer for all matrices
	m_cameraBuffer = nv_helpers_dx12::CreateBuffer(
        m_device.Get(), m_cameraBufferSize, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);

	// Create a descriptor heap that will be used by the rasterization shaders
	m_constHeap = nv_helpers_dx12::CreateDescriptorHeap(
        m_device.Get(), 1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);

	// Describe and create the constant buffer view.
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	cbvDesc.BufferLocation = m_cameraBuffer->GetGPUVirtualAddress();
	cbvDesc.SizeInBytes = m_cameraBufferSize;

	// Get a handle to the heap memory on the CPU side, to be able to write the
	// descriptors directly
	const D3D12_CPU_DESCRIPTOR_HANDLE srvHandle =
        m_constHeap->GetCPUDescriptorHandleForHeapStart();
	m_device->CreateConstantBufferView(&cbvDesc, srvHandle);
}

// #DXR Extra: Perspective Camera
//--------------------------------------------------------------------------------
// Create and copies the viewmodel and perspective matrices of the camera
//
void RevEngineMain::UpdateCameraBuffer()
{
	// Copy the matrix contents
	uint8_t *pData;
	ThrowIfFailed(m_cameraBuffer->Map(0, nullptr, (void **)&pData));
	memcpy(pData, m_camera.m_matrices.data(), m_cameraBufferSize);
	m_cameraBuffer->Unmap(0, nullptr);
	
}

//-----------------------------------------------------------------------------
// 
// #DXR Extra: Per-Instance Data
void RevEngineMain::CreatePerInstanceConstantBuffers()
{
	// Due to HLSL packing rules, we create the CB with 9 float4 (each needs to start on a 16-byte
	// boundary)
	XMVECTOR bufferData[] = {
		// A
		XMVECTOR{1.0f, 0.0f, 0.0f, 1.0f},
        XMVECTOR{1.0f, 0.4f, 0.0f, 1.0f},
        XMVECTOR{1.f, 0.7f, 0.0f, 1.0f},

        // B
        XMVECTOR{0.0f, 1.0f, 0.0f, 1.0f},
        XMVECTOR{0.0f, 1.0f, 0.4f, 1.0f},
        XMVECTOR{0.0f, 1.0f, 0.7f, 1.0f},

        // C
        XMVECTOR{0.0f, 0.0f, 1.0f, 1.0f},
        XMVECTOR{0.4f, 0.0f, 1.0f, 1.0f},
        XMVECTOR{0.7f, 0.0f, 1.0f, 1.0f},
    };

	m_perInstanceConstantBuffers.resize(3);
	int i(0);
	for (auto& cb : m_perInstanceConstantBuffers)
	{
		const uint32_t bufferSize = sizeof(XMVECTOR) * 3;
		cb = nv_helpers_dx12::CreateBuffer(m_device.Get(), bufferSize, D3D12_RESOURCE_FLAG_NONE,
                                           D3D12_RESOURCE_STATE_GENERIC_READ,
                                           nv_helpers_dx12::kUploadHeapProps);
		uint8_t* pData;
		ThrowIfFailed(cb->Map(0, nullptr, (void**)&pData));
		memcpy(pData, &bufferData[i * 3], bufferSize);
		cb->Unmap(0, nullptr);
		++i;
	}
}
void RevEngineMain::CreateDepthBuffer()
{
	// The depth buffer heap type is specific for that usage, and the heap contents are not visible
	// from the shaders
	m_dsvHeap = nv_helpers_dx12::CreateDescriptorHeap(m_device.Get(), 1,
      D3D12_DESCRIPTOR_HEAP_TYPE_DSV, false);

	// The depth and stencil can be packed into a single 32-bit texture buffer. Since we do not need
	// stencil, we use the 32 bits to store depth information (DXGI_FORMAT_D32_FLOAT).
	D3D12_HEAP_PROPERTIES depthHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

	D3D12_RESOURCE_DESC depthResourceDesc =
      CD3DX12_RESOURCE_DESC::Tex2D(REV_DEPTH_STENCIL_FORMAT, m_windowData.m_width, m_windowData.m_height, 1, 1);
	depthResourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	// The depth values will be initialized to 1
	CD3DX12_CLEAR_VALUE depthOptimizedClearValue(REV_DEPTH_STENCIL_FORMAT, 1.0f, 0);

	// Allocate the buffer itself, with a state allowing depth writes
	ThrowIfFailed(m_device->CreateCommittedResource(
      &depthHeapProperties, D3D12_HEAP_FLAG_NONE, &depthResourceDesc,
      D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthOptimizedClearValue, IID_PPV_ARGS(&m_depthStencil)));

	// Write the depth buffer view into the depth buffer heap
	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Format = REV_DEPTH_STENCIL_FORMAT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

	m_device->CreateDepthStencilView(m_depthStencil.Get(), &dsvDesc,
      m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

// Helper function for setting the window's title text.
void RevEngineMain::SetCustomWindowText(LPCWSTR text)
{
	std::wstring windowText = m_windowData.m_title + L": " + text;
	SetWindowText(Win32Application::GetHwnd(), windowText.c_str());
}

_Use_decl_annotations_
void RevEngineMain::GetHardwareAdapter(
    IDXGIFactory1* pFactory,
    IDXGIAdapter1** ppAdapter,
    bool requestHighPerformanceAdapter)
{
    *ppAdapter = nullptr;

    ComPtr<IDXGIAdapter1> adapter;

    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(pFactory->QueryInterface(IID_PPV_ARGS(&factory6))))
    {
        for (
            UINT adapterIndex = 0;
            DXGI_ERROR_NOT_FOUND != factory6->EnumAdapterByGpuPreference(
                adapterIndex,
                requestHighPerformanceAdapter == true ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE : DXGI_GPU_PREFERENCE_UNSPECIFIED,
                IID_PPV_ARGS(&adapter));
            ++adapterIndex)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                // Don't select the Basic Render Driver adapter.
                // If you want a software adapter, pass in "/warp" on the command line.
                continue;
            }

            // Check to see whether the adapter supports Direct3D 12, but don't create the
            // actual device yet.
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
            {
                break;
            }
        }
    }
    else
    {
        for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != pFactory->EnumAdapters1(adapterIndex, &adapter); ++adapterIndex)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                // Don't select the Basic Render Driver adapter.
                // If you want a software adapter, pass in "/warp" on the command line.
                continue;
            }

            // Check to see whether the adapter supports Direct3D 12, but don't create the
            // actual device yet.
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
            {
                break;
            }
        }
    }
    
    *ppAdapter = adapter.Detach();
}
