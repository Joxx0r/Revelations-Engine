#pragma once

struct RevModelD3DData;
struct RevPSOInitializationData
{
	UINT m_nInputLayout = 0;
	UINT m_numRenderTargets = 1;
	struct RevShaderRasterizer* m_shader = nullptr;
	const D3D12_INPUT_ELEMENT_DESC* m_inputLayoutData = nullptr;
	struct ID3D12RootSignature* m_rootSignature = nullptr;
	struct ID3D12PipelineState** m_pso = nullptr;
	D3D12_BLEND_DESC* m_blendDesc = nullptr;
	D3D12_RASTERIZER_DESC* m_rasterizerDesc = nullptr;
	D3D12_DEPTH_STENCIL_DESC* m_depthStencilDesc = nullptr;
	DXGI_FORMAT* m_rtvFormats = nullptr;

	bool m_useDepth = true;
	bool m_useStencil = true;
};

class RevUtils
{
public:

	static UINT CalcConstantBufferByteSize(UINT byteSize)
	{
		// Constant buffers must be a multiple of the minimum hardware
		// allocation size (usually 256 bytes).  So round up to nearest
		// multiple of 256.  We do this by adding 255 and then masking off
		// the lower 2 bytes which store all bits < 256.
		// Example: Suppose byteSize = 300.
		// (300 + 255) & ~255
		// 555 & ~255
		// 0x022B & ~0x00ff
		// 0x022B & 0xff00
		// 0x0200
		// 512
		return (byteSize + 255) & ~255;
	}

	static struct ID3D12Resource* CreateDefaultBuffer(
		ID3D12Device5* device,
		ID3D12GraphicsCommandList* cmdList,
		const void* initData,
		UINT64 byteSize,
		ID3D12Resource* uploadBuffer);

	static void CreateModelRootDescription(
		CD3DX12_ROOT_PARAMETER* parameter,
        UINT nParameters,
        RevModelD3DData& outData);

	static void CreatePSO(
		RevPSOInitializationData& initializationData);

	static float Clamp(float value, float low, float high)
	{
		return value < low ? low : (value > high ? high : value);
	}
};