#pragma once

#include "IBuffer.h"

class UniformBufferGL : public BufferGL, public IUniformBuffer
{
public:
	void destroy() override
	{
		BufferGL::destroy();
	}

	void update(void* data, unsigned size, unsigned offset) override
	{
		/*void* ptr = glMapBufferRange( GL_UNIFORM_BUFFER, offset, size, GL_MAP_WRITE_BIT );
	  memcpy( ptr, data, size );
	  glUnmapBuffer( GL_UNIFORM_BUFFER );*/

		glBindBuffer(GL_UNIFORM_BUFFER, id);
		glBufferSubData(GL_UNIFORM_BUFFER, offset, size, data);
	}

	void getSubData(void* data, unsigned size, unsigned offset) override
	{
		BufferGL::getSubData(data, size, offset);
	}

	rDesc getDesc() override
	{
		return BufferGL::getDesc();
	}
};