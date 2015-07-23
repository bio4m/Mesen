#include "stdafx.h"
#include "Renderer.h"
#include "DirectXTK/SpriteBatch.h"
#include "DirectXTK/SpriteFont.h"
#include "DirectXTK/DDSTextureLoader.h"
#include "DirectXTK/WICTextureLoader.h"
#include "../Core/PPU.h"
#include "../Core/VideoDecoder.h"
#include "../Core/EmulationSettings.h"
#include "../Core/MessageManager.h"
#include "../Utilities/UTF8Util.h"

using namespace DirectX;

namespace NES 
{
	Renderer::Renderer(HWND hWnd)
	{
		SetScreenSize(256, 240);

		_hWnd = hWnd;

		if(FAILED(InitDevice())) {
			CleanupDevice();
		} else {
			PPU::RegisterVideoDevice(this);
			MessageManager::RegisterMessageManager(this);
		}
	}

	Renderer::~Renderer()
	{
		CleanupDevice();
	}

	void Renderer::SetScreenSize(uint32_t screenWidth, uint32_t screenHeight)
	{
		_screenWidth = screenWidth;
		_screenHeight = screenHeight;
		_bytesPerPixel = 4;

		_hdScreenWidth = _screenWidth * 4;
		_hdScreenHeight = (_screenHeight - 16) * 4;
		
		_screenBufferSize = _screenWidth * _screenHeight * _bytesPerPixel;
		_hdScreenBufferSize = _hdScreenWidth * _hdScreenHeight * _bytesPerPixel;
	}

	void Renderer::CleanupDevice()
	{
		if(_pTexture) _pTexture->Release();
		if(_overlayTexture) _overlayTexture->Release();
		if(_toastTexture) { _toastTexture->Release(); }

		if(_samplerState) _samplerState->Release();
		if(_pRenderTargetView) _pRenderTargetView->Release();
		if(_pSwapChain) _pSwapChain->Release();
		if(_pDeviceContext) _pDeviceContext->ClearState();
		if(_pDeviceContext1) _pDeviceContext1->Release();
		if(_pd3dDevice1) _pd3dDevice1->Release();
		if(_pd3dDevice) _pd3dDevice->Release();
		if(_pAlphaEnableBlendingState) _pAlphaEnableBlendingState->Release();
		if(_pDepthDisabledStencilState) _pDepthDisabledStencilState->Release();

		if(_videoRAM) {
			delete[] _videoRAM;
			_videoRAM = nullptr;
		}

		if(_nextFrameBuffer) {
			delete[] _nextFrameBuffer;
			_nextFrameBuffer = nullptr;
		}

		if(_overlayBuffer) {
			delete[] _overlayBuffer;
			_overlayBuffer = nullptr;
		}

		if(_ppuOutputBuffer) {
			delete[] _ppuOutputBuffer;
		}

		if(_ppuOutputSecondaryBuffer) {
			delete[] _ppuOutputSecondaryBuffer;
		}
	}

	//--------------------------------------------------------------------------------------
	// Create Direct3D device and swap chain
	//--------------------------------------------------------------------------------------
	HRESULT Renderer::InitDevice()
	{
		HRESULT hr = S_OK;

		UINT createDeviceFlags = 0;
#ifdef _DEBUG
		createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

		D3D_DRIVER_TYPE driverTypes[] =
		{
			D3D_DRIVER_TYPE_HARDWARE,
			D3D_DRIVER_TYPE_WARP,
			D3D_DRIVER_TYPE_REFERENCE,
		};
		UINT numDriverTypes = ARRAYSIZE(driverTypes);

		D3D_FEATURE_LEVEL featureLevels[] =
		{
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0,
		};
		UINT numFeatureLevels = ARRAYSIZE(featureLevels);

		DXGI_SWAP_CHAIN_DESC sd;
		ZeroMemory(&sd, sizeof(sd));
		sd.BufferCount = 1;
		sd.BufferDesc.Width = _hdScreenWidth;
		sd.BufferDesc.Height = _hdScreenHeight;
		sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.BufferDesc.RefreshRate.Numerator = 60;
		sd.BufferDesc.RefreshRate.Denominator = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow = _hWnd;
		sd.SampleDesc.Count = 1;
		sd.SampleDesc.Quality = 0;
		sd.Windowed = TRUE;

		for(UINT driverTypeIndex = 0; driverTypeIndex < numDriverTypes; driverTypeIndex++) {
			_driverType = driverTypes[driverTypeIndex];
			hr = D3D11CreateDeviceAndSwapChain(nullptr, _driverType, nullptr, createDeviceFlags, featureLevels, numFeatureLevels,
				D3D11_SDK_VERSION, &sd, &_pSwapChain, &_pd3dDevice, &_featureLevel, &_pDeviceContext);

			if(hr == E_INVALIDARG) {
				// DirectX 11.0 platforms will not recognize D3D_FEATURE_LEVEL_11_1 so we need to retry without it
				hr = D3D11CreateDeviceAndSwapChain(nullptr, _driverType, nullptr, createDeviceFlags, &featureLevels[1], numFeatureLevels - 1,
					D3D11_SDK_VERSION, &sd, &_pSwapChain, &_pd3dDevice, &_featureLevel, &_pDeviceContext);
			}

			if(SUCCEEDED(hr)) {
				break;
			}
		}
		if(FAILED(hr)) {
			return hr;
		}

		// Obtain the Direct3D 11.1 versions if available
		hr = _pd3dDevice->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&_pd3dDevice1));
		if(SUCCEEDED(hr)) {
			(void)_pDeviceContext->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&_pDeviceContext1));
		}

		// Create a render target view
		ID3D11Texture2D* pBackBuffer = nullptr;
		hr = _pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
		if(FAILED(hr)) {
			return hr;
		}

		hr = _pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &_pRenderTargetView);
		pBackBuffer->Release();
		if(FAILED(hr)) {
			return hr;
		}

		D3D11_DEPTH_STENCIL_DESC depthDisabledStencilDesc;
		ZeroMemory(&depthDisabledStencilDesc, sizeof(depthDisabledStencilDesc));
		depthDisabledStencilDesc.DepthEnable = false;
		depthDisabledStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthDisabledStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;
		depthDisabledStencilDesc.StencilEnable = true;
		depthDisabledStencilDesc.StencilReadMask = 0xFF;
		depthDisabledStencilDesc.StencilWriteMask = 0xFF;
		depthDisabledStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		depthDisabledStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
		depthDisabledStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		depthDisabledStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		depthDisabledStencilDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		depthDisabledStencilDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
		depthDisabledStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		depthDisabledStencilDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

		// Create the state using the device.
		if(FAILED(_pd3dDevice->CreateDepthStencilState(&depthDisabledStencilDesc, &_pDepthDisabledStencilState))) {
			return false;
		}

		// Clear the blend state description.
		D3D11_BLEND_DESC blendStateDescription;
		ZeroMemory(&blendStateDescription, sizeof(D3D11_BLEND_DESC));

		// Create an alpha enabled blend state description.
		blendStateDescription.RenderTarget[0].BlendEnable = TRUE;
		blendStateDescription.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blendStateDescription.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendStateDescription.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendStateDescription.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendStateDescription.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		blendStateDescription.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendStateDescription.RenderTarget[0].RenderTargetWriteMask = 0x0f;

		// Create the blend state using the description.
		if(FAILED(_pd3dDevice->CreateBlendState(&blendStateDescription, &_pAlphaEnableBlendingState))) {
			return false;
		}

		float blendFactor[4];
		blendFactor[0] = 0.0f;
		blendFactor[1] = 0.0f;
		blendFactor[2] = 0.0f;
		blendFactor[3] = 0.0f;
	
		_pDeviceContext->OMSetBlendState(_pAlphaEnableBlendingState, blendFactor, 0xffffffff);
		_pDeviceContext->OMSetDepthStencilState(_pDepthDisabledStencilState, 1);
		_pDeviceContext->OMSetRenderTargets(1, &_pRenderTargetView, nullptr);

		// Setup the viewport
		D3D11_VIEWPORT vp;
		vp.Width = (FLOAT)_hdScreenWidth;
		vp.Height = (FLOAT)_hdScreenHeight;
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		vp.TopLeftX = 0;
		vp.TopLeftY = 0;
		_pDeviceContext->RSSetViewports(1, &vp);

		_videoRAM = new uint8_t[_screenBufferSize];
		_nextFrameBuffer = new uint8_t[_screenBufferSize];
		_ppuOutputBuffer = new uint16_t[_screenWidth * _screenHeight];
		_ppuOutputSecondaryBuffer = new uint16_t[_screenWidth * _screenHeight];
		memset(_videoRAM, 0x00, _screenBufferSize);
		memset(_nextFrameBuffer, 0x00, _screenBufferSize);
		memset(_ppuOutputBuffer, 0x00, _screenWidth * _screenHeight * sizeof(uint16_t));
		memset(_ppuOutputSecondaryBuffer, 0x00, _screenWidth * _screenHeight * sizeof(uint16_t));

		_pTexture = CreateTexture(_screenWidth, _screenHeight);
		if(!_pTexture) {
			return 0;
		}

		_overlayBuffer = new uint8_t[_hdScreenBufferSize];  //High res overlay for UI elements (4x res)
		memset(_overlayBuffer, 0x00, _hdScreenBufferSize);

		_overlayTexture = CreateTexture(_hdScreenWidth, _hdScreenHeight);
		if(!_overlayTexture) {
			return 0;
		}

		////////////////////////////////////////////////////////////////////////////
		_spriteBatch.reset(new SpriteBatch(_pDeviceContext));

		_smallFont.reset(new SpriteFont(_pd3dDevice, L"Resources\\Roboto.9.spritefont"));
		_font.reset(new SpriteFont(_pd3dDevice, L"Resources\\Roboto.12.spritefont"));

		//Sample state
		D3D11_SAMPLER_DESC samplerDesc;
		ZeroMemory(&samplerDesc, sizeof(samplerDesc));
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		//samplerDesc.BorderColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		samplerDesc.MinLOD = -FLT_MAX;
		samplerDesc.MaxLOD = FLT_MAX;
		samplerDesc.MipLODBias = 0.0f;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		
		_pd3dDevice->CreateSamplerState(&samplerDesc, &_samplerState);

		if(!FAILED(CreateDDSTextureFromFile(_pd3dDevice, L"Resources\\Toast.dds", nullptr, &_toastTexture))) {
			return 0;
		}

		/*
		ID3DBlob* vertexShaderBlob = nullptr;
		ID3D11VertexShader* vertexShader = nullptr;
		CompileShader(L"PixelShader.hlsl", "VS", "vs_4_0", &vertexShaderBlob);
		if(vertexShaderBlob) {
			_pd3dDevice->CreateVertexShader(vertexShaderBlob->GetBufferPointer(), vertexShaderBlob->GetBufferSize(), nullptr, &vertexShader);
			_pDeviceContext->VSSetShader(vertexShader, nullptr, 0);
			//vertexShaderBlob->Release();
		}
		
		ID3DBlob* pixelShaderBlob = nullptr;		
		CompileShader(L"PixelShader.hlsl", "PShader", "ps_4_0", &pixelShaderBlob);
		if(pixelShaderBlob) {
			_pd3dDevice->CreatePixelShader(pixelShaderBlob->GetBufferPointer(), pixelShaderBlob->GetBufferSize(), nullptr, &_pixelShader);
			//pixelShaderBlob->Release();
		}*/

		return S_OK;
	}

	ID3D11Texture2D* Renderer::CreateTexture(uint32_t width, uint32_t height)
	{
		ID3D11Texture2D* texture;

		UINT fred;
		_pd3dDevice->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, 16, &fred);

		D3D11_TEXTURE2D_DESC desc;
		ZeroMemory(&desc, sizeof(D3D11_TEXTURE2D_DESC));
		desc.ArraySize = 1;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.MipLevels = 1;
		desc.MiscFlags = 0;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = fred;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.Width = width;
		desc.Height = height;
		desc.MiscFlags = 0;

		if(FAILED(_pd3dDevice->CreateTexture2D(&desc, nullptr, &texture))) {
			return nullptr;
		}
		return texture;
	}

	ID3D11ShaderResourceView* Renderer::GetShaderResourceView(ID3D11Texture2D* texture)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		D3D11_TEXTURE2D_DESC desc;
		D3D11_RESOURCE_DIMENSION type;
		texture->GetType(&type);
		texture->GetDesc(&desc);
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = desc.MipLevels;
		srvDesc.Texture2D.MostDetailedMip = desc.MipLevels - 1;

		ID3D11ShaderResourceView *shaderResourceView = nullptr;
		_pd3dDevice->CreateShaderResourceView(texture, &srvDesc, &shaderResourceView);

		return shaderResourceView;
	}

	void Renderer::DisplayMessage(string title, string message)
	{
		shared_ptr<ToastInfo> toast(new ToastInfo(title, message, 4000, "Resources\\MesenIcon.bmp"));
		DisplayToast(toast);
	}

	void Renderer::DisplayToast(shared_ptr<ToastInfo> toast)
	{
		_toasts.push_front(toast);
	}

	void Renderer::DrawOutlinedString(string message, float x, float y, DirectX::FXMVECTOR color, float scale)
	{
		SpriteBatch* spritebatch = _spriteBatch.get();
		std::wstring textStr = utf8::utf8::decode(message);
		const wchar_t *text = textStr.c_str();

		for(uint8_t offsetX = 2; offsetX > 0; offsetX--) {
			for(uint8_t offsetY = 2; offsetY > 0; offsetY--) {
				_font->DrawString(spritebatch, text, XMFLOAT2(x + offsetX, y + offsetY), Colors::Black, 0.0f, XMFLOAT2(0, 0), scale);
				_font->DrawString(spritebatch, text, XMFLOAT2(x - offsetX, y + offsetY), Colors::Black, 0.0f, XMFLOAT2(0, 0), scale);
				_font->DrawString(spritebatch, text, XMFLOAT2(x + offsetX, y - offsetY), Colors::Black, 0.0f, XMFLOAT2(0, 0), scale);
				_font->DrawString(spritebatch, text, XMFLOAT2(x - offsetX, y - offsetY), Colors::Black, 0.0f, XMFLOAT2(0, 0), scale);
				_font->DrawString(spritebatch, text, XMFLOAT2(x + offsetX, y), Colors::Black, 0.0f, XMFLOAT2(0, 0), scale);
				_font->DrawString(spritebatch, text, XMFLOAT2(x - offsetX, y), Colors::Black, 0.0f, XMFLOAT2(0, 0), scale);
				_font->DrawString(spritebatch, text, XMFLOAT2(x, y + offsetY), Colors::Black, 0.0f, XMFLOAT2(0, 0), scale);
				_font->DrawString(spritebatch, text, XMFLOAT2(x, y - offsetY), Colors::Black, 0.0f, XMFLOAT2(0, 0), scale);
			}
		}
		_font->DrawString(spritebatch, text, XMFLOAT2(x, y), color, 0.0f, XMFLOAT2(0, 0), scale);
	}

	void Renderer::DrawNESScreen()
	{
		_frameLock.Acquire();
		memcpy(_ppuOutputSecondaryBuffer, _ppuOutputBuffer, 256 * 240 * sizeof(uint16_t));
		_frameLock.Release();

		VideoDecoder::DecodeFrame(_ppuOutputSecondaryBuffer, (uint32_t*)_nextFrameBuffer);

		RECT sourceRect;
		sourceRect.left = 0;
		sourceRect.right = _screenWidth;
		sourceRect.top = 8;
		sourceRect.bottom = _screenHeight - 8;

		RECT destRect;
		destRect.left = 0;
		destRect.top = 0;
		destRect.right = _screenWidth * 4;
		destRect.bottom = (_screenHeight - 16) * 4;
		XMVECTOR position{ { 0, 0 } };

		D3D11_MAPPED_SUBRESOURCE dd;
		dd.pData = (void *)_videoRAM;
		dd.RowPitch = _screenWidth * _bytesPerPixel;
		dd.DepthPitch = _screenBufferSize;

		_pDeviceContext->Map(_pTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &dd);
		memcpy(dd.pData, _nextFrameBuffer, _screenBufferSize);
		_pDeviceContext->Unmap(_pTexture, 0);

		ID3D11ShaderResourceView *nesOutputBuffer = GetShaderResourceView(_pTexture);
		_spriteBatch->Draw(nesOutputBuffer, destRect, &sourceRect);
		nesOutputBuffer->Release();
	}

	void Renderer::DrawPauseScreen()
	{
		RECT destRect;
		destRect.left = 0;
		destRect.right = _hdScreenWidth;
		destRect.bottom = _hdScreenHeight;
		destRect.top = 0;

		XMVECTOR position{ { 0, 0 } };

		D3D11_MAPPED_SUBRESOURCE dd;
		dd.pData = (void *)_overlayBuffer;
		dd.RowPitch = _hdScreenWidth * _bytesPerPixel;
		dd.DepthPitch = _hdScreenBufferSize;

		_pDeviceContext->Map(_overlayTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &dd);
		for(uint32_t i = 0, len = _hdScreenHeight*_hdScreenWidth; i < len; i++) {
			//Gray transparent overlay
			((uint32_t*)dd.pData)[i] = 0x99222222;
		}
		_pDeviceContext->Unmap(_overlayTexture, 0);
		
		ID3D11ShaderResourceView *shaderResourceView = GetShaderResourceView(_overlayTexture);
		_spriteBatch->Draw(shaderResourceView, destRect); // , position, &sourceRect, Colors::White, 0.0f, position, 4.0f);
		shaderResourceView->Release();

		DrawOutlinedString("PAUSED", (float)_hdScreenWidth / 2 - 145, (float)_hdScreenHeight / 2 - 47, Colors::AntiqueWhite, 4.5f);
	}

	void Renderer::Render()
	{
		if(_frameChanged || EmulationSettings::CheckFlag(EmulationFlags::Paused) || !_toasts.empty()) {
			_frameChanged = false;
			// Clear the back buffer 
			_pDeviceContext->ClearRenderTargetView(_pRenderTargetView, Colors::Black);

			_spriteBatch->Begin(SpriteSortMode_Deferred, nullptr, _samplerState, nullptr, nullptr, [=] {
				//_pDeviceContext->PSSetShader(_pixelShader, 0, 0);
			});

			//Draw nes screen
			DrawNESScreen();

			/*_spriteBatch->End();

			_spriteBatch->Begin(SpriteSortMode_Deferred, nullptr, _samplerState);*/

			if(EmulationSettings::CheckFlag(EmulationFlags::Paused)) {
				DrawPauseScreen();
			} else {
				//Draw FPS counter
				if(EmulationSettings::CheckFlag(EmulationFlags::ShowFPS)) {				
					if(_fpsTimer.GetElapsedMS() > 1000) {
						//Update fps every sec
						if(_frameCount - _lastFrameCount < 0) {
							_currentFPS = 0;
						} else {
							_currentFPS = (int)(std::round((double)(_frameCount - _lastFrameCount) / (_fpsTimer.GetElapsedMS() / 1000)));
						}
						_lastFrameCount = _frameCount;
						_fpsTimer.Reset();
					}

					string fpsString = string("FPS: ") + std::to_string(_currentFPS);
					DrawOutlinedString(fpsString, 256 * 4 - 80, 13, Colors::AntiqueWhite, 1.0f);
				}
			}

			DrawToasts();

			_spriteBatch->End();

			// Present the information rendered to the back buffer to the front buffer (the screen)
			_pSwapChain->Present(0, 0);
		}
	}

	void Renderer::RemoveOldToasts()
	{
		_toasts.remove_if([](shared_ptr<ToastInfo> toast) { return toast->IsToastExpired(); });
	}

	void Renderer::DrawToasts()
	{
		RemoveOldToasts();

		int counter = 0;
		for(shared_ptr<ToastInfo> toast : _toasts) {
			if(counter < 3) {
				DrawToast(toast, counter);
			} else {
				break;
			}
			counter++;
		}
	}

	std::wstring Renderer::WrapText(string utf8Text, SpriteFont* font, float maxLineWidth)
	{
		using std::wstring;
		wstring text = utf8::utf8::decode(utf8Text);
		wstring wrappedText;
		list<wstring> words;
		wstring currentWord;
		for(size_t i = 0, len = text.length(); i < len; i++) {
			if(text[i] == L' ' || text[i] == L'\n') {
				if(currentWord.length() > 0) {
					words.push_back(currentWord);
					currentWord.clear();
				}
			} else {
				currentWord += text[i];
			}
		}
		if(currentWord.length() > 0) {
			words.push_back(currentWord);
		}

		float spaceWidth = font->MeasureString(L" ").m128_f32[0];
		float lineWidth = 0.0f;
		for(wstring word : words) {
			for(unsigned int i = 0; i < word.size(); i++) {
				if(!font->ContainsCharacter(word[i])) {
					word[i] = L'?';
				}
			}
			float wordWidth = font->MeasureString(word.c_str()).m128_f32[0];

			if(lineWidth + wordWidth < maxLineWidth) {
				wrappedText += word + L" ";
				lineWidth += wordWidth + spaceWidth;
			} else {
				wrappedText += L"\n" + word + L" ";
				lineWidth = wordWidth + spaceWidth;
			}
		}

		return wrappedText;
	}

	void Renderer::DrawToast(shared_ptr<ToastInfo> toast, int posIndex)
	{
		RECT dest;
		dest.top = _hdScreenHeight - (100 * (posIndex + 1)) - 50;
		dest.left = (_hdScreenWidth - 340) / 2;
		dest.bottom = dest.top + 70;
		dest.right = dest.left + 340;

		//Get opacity for fade in/out effect
		float opacity = toast->GetOpacity();
		XMVECTORF32 color = { opacity, opacity, opacity, opacity };

		_spriteBatch->Draw(_toastTexture, dest, color);

		float textLeftMargin = 10.0f;
		if(toast->HasIcon()) {
			ID3D11ShaderResourceView* icon;
			if(!FAILED(CreateWICTextureFromMemory(_pd3dDevice, toast->GetToastIcon(), toast->GetIconSize(), nullptr, &icon))) {
				RECT iconRect;
				iconRect.top = dest.top + 3;
				iconRect.bottom = dest.bottom - 3;
				iconRect.left = dest.left + 3;
				iconRect.right = iconRect.left + 64;
				_spriteBatch->Draw(icon, iconRect, color);
				textLeftMargin = 75.0f;
				icon->Release();
			}
		}

		_smallFont->DrawString(_spriteBatch.get(), WrapText(toast->GetToastTitle(), _smallFont.get(), 340 - 30 - textLeftMargin).c_str(), XMFLOAT2(dest.left + textLeftMargin - 5.0f, dest.top + 5.0f), color);
		_font->DrawString(_spriteBatch.get(), WrapText(toast->GetToastMessage(), _font.get(), 340 - 30 - textLeftMargin).c_str(), XMFLOAT2(dest.left + textLeftMargin - 2.0f, dest.top + 19.0f), color);
	}

	void Renderer::UpdateFrame(void* frameBuffer)
	{
		_frameLock.Acquire();
		memcpy(_ppuOutputBuffer, frameBuffer, 256 * 240 * 2);
		_frameLock.Release();

		_frameChanged = true;
		_frameCount++;
	}

	void Renderer::TakeScreenshot(string romFilename)
	{
		uint32_t* frameBuffer = new uint32_t[256 * 240];
			
		_frameLock.Acquire();
		memcpy(frameBuffer, _nextFrameBuffer, 256 * 240 * 4);
		_frameLock.Release();

		//ARGB -> ABGR
		for(uint32_t i = 0; i < 256 * 240; i++) {
			frameBuffer[i] = (frameBuffer[i] & 0xFF00FF00) | ((frameBuffer[i] & 0xFF0000) >> 16) | ((frameBuffer[i] & 0xFF) << 16);
		}

		int counter = 0;
		string baseFilename = FolderUtilities::GetScreenshotFolder() + FolderUtilities::GetFilename(romFilename, false);
		string ssFilename;
		while(true) {
			string counterStr = std::to_string(counter);
			while(counterStr.length() < 3) {
				counterStr = "0" + counterStr;
			}
			ssFilename = baseFilename + "_" + counterStr + ".png";
			ifstream file(ssFilename, ios::in);
			if(file) {
				file.close();
			} else {
				break;
			}
			counter++;
		}

		PNGWriter::WritePNG(ssFilename, (uint8_t*)frameBuffer, 256, 240);	
		MessageManager::DisplayMessage("Screenshot saved", FolderUtilities::GetFilename(ssFilename, true));
	}

}