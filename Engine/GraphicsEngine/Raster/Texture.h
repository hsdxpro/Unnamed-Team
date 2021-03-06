#pragma once
#include "GraphicsEngine/ITexture.h"

class ITextureGapi;
class IGapi;


class Texture : public graphics::ITexture 
{
public:
	Texture(IGapi* gapi);
	~Texture();

	void Acquire();
	void Release() override;

	bool Load(const std::string& file_path) override;

	void Reset() override;

	ITextureGapi* GetTexture();

private:
	size_t refcount;

	// gpu resource
	IGapi* gapi;
	ITextureGapi* tex;
};