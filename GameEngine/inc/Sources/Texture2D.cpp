#include "enginepch.h"
#include "Debug.h"
#include "Texture2D.h"
#include "GraphicDevice.h"

namespace GameEngine
{
	using namespace std;

	void Texture2D::LoadFromFile(const wstring & path)
	{
		Debug::Log(L"#Load : " + path);
		isValid = false;
		auto& device = GraphicDevice::Instance()->device;
		HRESULT hr = D3DX11CreateTextureFromFile(device, path.c_str(), 0, 0, (ID3D11Resource**)&texture2D.p, 0);
		if(FAILED(hr)) {
			Debug::Log("Failed(D3DXCreateTextureFromFile)", Debug::Red);
			return;
		}
		D3D11_TEXTURE2D_DESC texDesc;
		texture2D->GetDesc(&texDesc);
		width = texDesc.Width;
		height = texDesc.Height;
		hr = device->CreateShaderResourceView(texture2D, 0, &srv.p);
		if(FAILED(hr)) {
			Debug::Log("Failed(CreateShaderResourceView)", Debug::Red);
			return;
		}
		Debug::Log("Success", Debug::Green);
		isValid = true;
	}

	void Texture2D::CreateTexture2D(int width, int height, TextureFormat format, Usage usage, BindFlag bindFlag, CpuAccess cpuflag)
	{
		D3D11_TEXTURE2D_DESC texDesc;
		texDesc.Width = (UINT)width;
		texDesc.Height = (UINT)height;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = (DXGI_FORMAT)format;
		texDesc.SampleDesc.Count = msaa;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Usage = (D3D11_USAGE)usage;
		texDesc.BindFlags = (UINT)bindFlag;
		texDesc.CPUAccessFlags = cpuflag;
		texDesc.MiscFlags = 0;

		auto& device = GraphicDevice::Instance()->device;

		HRESULT hr = device->CreateTexture2D(&texDesc, 0, &texture2D.p);
		if(FAILED(hr)) {
			Debug::Log(ToString() + " - failed to create Texture2D");
			return;
		}

		if(bindFlag & D3D11_BIND_SHADER_RESOURCE) {
			hr = device->CreateShaderResourceView(texture2D, 0, &srv.p);
			if(FAILED(hr)) {
				Debug::Log(ToString() + " - failed to create SRV");
				return;
			}
		}
		isValid = true;
	}
}