#include "stdafx_d3d12.h"
#ifdef _WIN32
#include "D3D12GSRender.h"
#include "d3dx12.h"
#include "../Common/TextureUtils.h"
// For clarity this code deals with texture but belongs to D3D12GSRender class
#include "D3D12Formats.h"

namespace
{
D3D12_COMPARISON_FUNC get_sampler_compare_func[] =
{
	D3D12_COMPARISON_FUNC_NEVER,
	D3D12_COMPARISON_FUNC_LESS,
	D3D12_COMPARISON_FUNC_EQUAL,
	D3D12_COMPARISON_FUNC_LESS_EQUAL,
	D3D12_COMPARISON_FUNC_GREATER,
	D3D12_COMPARISON_FUNC_NOT_EQUAL,
	D3D12_COMPARISON_FUNC_GREATER_EQUAL,
	D3D12_COMPARISON_FUNC_ALWAYS
};

D3D12_SAMPLER_DESC get_sampler_desc(const rsx::texture &texture) noexcept
{
	D3D12_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = get_texture_filter(texture.min_filter(), texture.mag_filter());
	samplerDesc.AddressU = get_texture_wrap_mode(texture.wrap_s());
	samplerDesc.AddressV = get_texture_wrap_mode(texture.wrap_t());
	samplerDesc.AddressW = get_texture_wrap_mode(texture.wrap_r());
	samplerDesc.ComparisonFunc = get_sampler_compare_func[texture.zfunc()];
	samplerDesc.MaxAnisotropy = get_texture_max_aniso(texture.max_aniso());
	samplerDesc.MipLODBias = texture.bias();
	samplerDesc.BorderColor[0] = (FLOAT)texture.border_color();
	samplerDesc.BorderColor[1] = (FLOAT)texture.border_color();
	samplerDesc.BorderColor[2] = (FLOAT)texture.border_color();
	samplerDesc.BorderColor[3] = (FLOAT)texture.border_color();
	samplerDesc.MinLOD = (FLOAT)(texture.min_lod() >> 8);
	samplerDesc.MaxLOD = (FLOAT)(texture.max_lod() >> 8);
	return samplerDesc;
}


/**
 * Create a texture residing in default heap and generate uploads commands in commandList,
 * using a temporary texture buffer.
 */
ComPtr<ID3D12Resource> upload_single_texture(
	const rsx::texture &texture,
	ID3D12Device *device,
	ID3D12GraphicsCommandList *command_list,
	DataHeap<ID3D12Resource, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT> &texture_buffer_heap)
{
	size_t w = texture.width(), h = texture.height();

	int format = texture.format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
	DXGI_FORMAT dxgi_format = get_texture_format(format);

	size_t buffer_size = get_placed_texture_storage_size(texture, 256);
	assert(texture_buffer_heap.canAlloc(buffer_size));
	size_t heap_offset = texture_buffer_heap.alloc(buffer_size);

	void *buffer;
	ThrowIfFailed(texture_buffer_heap.m_heap->Map(0, &CD3DX12_RANGE(heap_offset, heap_offset + buffer_size), &buffer));
	void *mapped_buffer = (char*)buffer + heap_offset;
	std::vector<MipmapLevelInfo> mipInfos = upload_placed_texture(texture, 256, mapped_buffer);
	texture_buffer_heap.m_heap->Unmap(0, &CD3DX12_RANGE(heap_offset, heap_offset + buffer_size));

	ComPtr<ID3D12Resource> result;
	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Tex2D(dxgi_format, (UINT)w, (UINT)h, 1, texture.mipmap()),
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(result.GetAddressOf())
		));

	size_t mip_level = 0;
	for (const MipmapLevelInfo mli : mipInfos)
	{
		command_list->CopyTextureRegion(&CD3DX12_TEXTURE_COPY_LOCATION(result.Get(), (UINT)mip_level), 0, 0, 0,
			&CD3DX12_TEXTURE_COPY_LOCATION(texture_buffer_heap.m_heap, { heap_offset + mli.offset, { dxgi_format, (UINT)mli.width, (UINT)mli.height, 1, (UINT)mli.rowPitch } }), nullptr);
		mip_level++;
	}

	command_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(result.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ));
	return result;
}

/**
*
*/
void update_existing_texture(
	const rsx::texture &texture,
	ID3D12GraphicsCommandList *command_list,
	DataHeap<ID3D12Resource, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT> &texture_buffer_heap,
	ID3D12Resource *existing_texture)
{
	size_t w = texture.width(), h = texture.height();

	int format = texture.format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
	DXGI_FORMAT dxgi_format = get_texture_format(format);

	size_t buffer_size = get_placed_texture_storage_size(texture, 256);
	assert(texture_buffer_heap.canAlloc(buffer_size));
	size_t heap_offset = texture_buffer_heap.alloc(buffer_size);

	void *buffer;
	ThrowIfFailed(texture_buffer_heap.m_heap->Map(0, &CD3DX12_RANGE(heap_offset, heap_offset + buffer_size), &buffer));
	void *mapped_buffer = (char*)buffer + heap_offset;
	std::vector<MipmapLevelInfo> mipInfos = upload_placed_texture(texture, 256, mapped_buffer);
	texture_buffer_heap.m_heap->Unmap(0, &CD3DX12_RANGE(heap_offset, heap_offset + buffer_size));

	command_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(existing_texture, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST));
	size_t miplevel = 0;
	for (const MipmapLevelInfo mli : mipInfos)
	{
		command_list->CopyTextureRegion(&CD3DX12_TEXTURE_COPY_LOCATION(existing_texture, (UINT)miplevel), 0, 0, 0,
			&CD3DX12_TEXTURE_COPY_LOCATION(texture_buffer_heap.m_heap, { heap_offset + mli.offset,{ dxgi_format, (UINT)mli.width, (UINT)mli.height, 1, (UINT)mli.rowPitch } }), nullptr);
		miplevel++;
	}

	command_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(existing_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ));
}
}

void D3D12GSRender::upload_and_bind_textures(ID3D12GraphicsCommandList *command_list, size_t descriptor_index, size_t texture_count)
{
	size_t used_texture = 0;

	for (u32 i = 0; i < rsx::limits::textures_count; ++i)
	{
		if (!textures[i].enabled()) continue;
		size_t w = textures[i].width(), h = textures[i].height();
		if (!w || !h) continue;

		const u32 texaddr = rsx::get_address(textures[i].offset(), textures[i].location());

		int format = textures[i].format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
		bool is_swizzled = !(textures[i].format() & CELL_GCM_TEXTURE_LN);

		ID3D12Resource *vram_texture;
		std::unordered_map<u32, ID3D12Resource* >::const_iterator ItRTT = m_rtts.m_renderTargets.find(texaddr);
		std::pair<TextureEntry, ComPtr<ID3D12Resource> > *cached_texture = m_textureCache.findDataIfAvailable(texaddr);
		bool isRenderTarget = false;
		if (ItRTT != m_rtts.m_renderTargets.end())
		{
			vram_texture = ItRTT->second;
			isRenderTarget = true;
		}
		else if (cached_texture != nullptr && (cached_texture->first == TextureEntry(format, w, h, textures[i].mipmap())))
		{
			if (cached_texture->first.m_isDirty)
			{
				update_existing_texture(textures[i], command_list, m_textureUploadData, cached_texture->second.Get());
				m_textureCache.protectData(texaddr, texaddr, get_texture_size(textures[i]));
			}
			vram_texture = cached_texture->second.Get();
		}
		else
		{
			if (cached_texture != nullptr)
				getCurrentResourceStorage().m_dirtyTextures.push_back(m_textureCache.removeFromCache(texaddr));
			ComPtr<ID3D12Resource> tex = upload_single_texture(textures[i], m_device.Get(), command_list, m_textureUploadData);
			vram_texture = tex.Get();
			m_textureCache.storeAndProtectData(texaddr, texaddr, get_texture_size(textures[i]), format, w, h, textures[i].mipmap(), tex);
		}

		D3D12_SHADER_RESOURCE_VIEW_DESC shared_resource_view_desc = {};
		shared_resource_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		shared_resource_view_desc.Format = get_texture_format(format);
		shared_resource_view_desc.Texture2D.MipLevels = textures[i].mipmap();

		switch (format)
		{
		case CELL_GCM_TEXTURE_COMPRESSED_HILO8:
		case CELL_GCM_TEXTURE_COMPRESSED_HILO_S8:
		case ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN) & CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
		case ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN) & CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8:
		default:
			LOG_ERROR(RSX, "Unimplemented Texture format : %x", format);
			break;
		case CELL_GCM_TEXTURE_B8:
			shared_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0);
			break;
		case CELL_GCM_TEXTURE_A1R5G5B5:
		case CELL_GCM_TEXTURE_A4R4G4B4:
		case CELL_GCM_TEXTURE_R5G6B5:
			shared_resource_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			break;
		case CELL_GCM_TEXTURE_A8R8G8B8:
		{


			u8 remap_a = textures[i].remap() & 0x3;
			u8 remap_r = (textures[i].remap() >> 2) & 0x3;
			u8 remap_g = (textures[i].remap() >> 4) & 0x3;
			u8 remap_b = (textures[i].remap() >> 6) & 0x3;
			if (isRenderTarget)
			{
				// ARGB format
				// Data comes from RTT, stored as RGBA already
				const int RemapValue[4] =
				{
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2
				};

				shared_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
					RemapValue[remap_r],
					RemapValue[remap_g],
					RemapValue[remap_b],
					RemapValue[remap_a]);
			}
			else
			{
				// ARGB format
				// Data comes from RSX mem, stored as ARGB already
				const int RemapValue[4] =
				{
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
					D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3
				};

				shared_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
					RemapValue[remap_r],
					RemapValue[remap_g],
					RemapValue[remap_b],
					RemapValue[remap_a]);
			}

			break;
		}
		case CELL_GCM_TEXTURE_COMPRESSED_DXT1:
		case CELL_GCM_TEXTURE_COMPRESSED_DXT23:
		case CELL_GCM_TEXTURE_COMPRESSED_DXT45:
		case CELL_GCM_TEXTURE_G8B8:
		case CELL_GCM_TEXTURE_R6G5B5:
		case CELL_GCM_TEXTURE_DEPTH24_D8:
		case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
		case CELL_GCM_TEXTURE_DEPTH16:
		case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
		case CELL_GCM_TEXTURE_X16:
		case CELL_GCM_TEXTURE_Y16_X16:
		case CELL_GCM_TEXTURE_R5G5B5A1:
		case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT:
		case CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT:
		case CELL_GCM_TEXTURE_X32_FLOAT:
		case CELL_GCM_TEXTURE_D1R5G5B5:
			shared_resource_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			break;
		case CELL_GCM_TEXTURE_D8R8G8B8:
		{
			const int RemapValue[4] =
			{
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
				D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3,
				D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1
			};

			u8 remap_a = textures[i].remap() & 0x3;
			u8 remap_r = (textures[i].remap() >> 2) & 0x3;
			u8 remap_g = (textures[i].remap() >> 4) & 0x3;
			u8 remap_b = (textures[i].remap() >> 6) & 0x3;

			shared_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
				RemapValue[remap_a],
				RemapValue[remap_r],
				RemapValue[remap_g],
				RemapValue[remap_b]);
			break;
		}
		case CELL_GCM_TEXTURE_Y16_X16_FLOAT:
			shared_resource_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			break;
		case CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
			shared_resource_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			break;
		case CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8:
			shared_resource_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			break;
		}

		m_device->CreateShaderResourceView(vram_texture, &shared_resource_view_desc,
			CD3DX12_CPU_DESCRIPTOR_HANDLE(getCurrentResourceStorage().m_descriptorsHeap->GetCPUDescriptorHandleForHeapStart())
			.Offset((UINT)descriptor_index + (UINT)used_texture, g_descriptorStrideSRVCBVUAV));

		if (getCurrentResourceStorage().m_currentSamplerIndex + 16 > 2048)
		{
			getCurrentResourceStorage().m_samplerDescriptorHeapIndex = 1;
			getCurrentResourceStorage().m_currentSamplerIndex = 0;
		}
		m_device->CreateSampler(&get_sampler_desc(textures[i]),
			CD3DX12_CPU_DESCRIPTOR_HANDLE(getCurrentResourceStorage().m_samplerDescriptorHeap[getCurrentResourceStorage().m_samplerDescriptorHeapIndex]->GetCPUDescriptorHandleForHeapStart())
			.Offset((UINT)getCurrentResourceStorage().m_currentSamplerIndex + (UINT)used_texture, g_descriptorStrideSamplers));

		used_texture++;
	}

	// Now fill remaining texture slots with dummy texture/sampler
	for (; used_texture < texture_count; used_texture++)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC shader_resource_view_desc = {};
		shader_resource_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		shader_resource_view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		shader_resource_view_desc.Texture2D.MipLevels = 1;
		shader_resource_view_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
			D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
			D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
			D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
			D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
		m_device->CreateShaderResourceView(m_dummyTexture, &shader_resource_view_desc,
			CD3DX12_CPU_DESCRIPTOR_HANDLE(getCurrentResourceStorage().m_descriptorsHeap->GetCPUDescriptorHandleForHeapStart())
			.Offset((INT)descriptor_index + (INT)used_texture, g_descriptorStrideSRVCBVUAV)
			);

		D3D12_SAMPLER_DESC sampler_desc = {};
		sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		m_device->CreateSampler(&sampler_desc,
			CD3DX12_CPU_DESCRIPTOR_HANDLE(getCurrentResourceStorage().m_samplerDescriptorHeap[getCurrentResourceStorage().m_samplerDescriptorHeapIndex]->GetCPUDescriptorHandleForHeapStart())
			.Offset((INT)getCurrentResourceStorage().m_currentSamplerIndex + (INT)used_texture, g_descriptorStrideSamplers)
			);
	}
}
#endif
