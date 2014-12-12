#include "Mesh.h"

#include <memory>
#include <iostream> // only for debug!!
#include <algorithm>
#include <vector>

#include <GraphicsApi>
#include <mymath/mymath.h>

using namespace mymath;
using namespace std;


inline static int fast_log2(unsigned n) {
	int i = -1;
	while (n != 0) {
		n >>= 1;
		i++;
	}
	return i;
}


////////////////////////////////////////////////////////////////////////////////
// Constructor & destructor

Mesh::Mesh(IGapi* gapi) : gapi(gapi) {
	refcount = 1;
	ib = nullptr;

	num_elements = 0;
	num_streams = 0;

	for (auto& stream : streams) {
		stream.vb = nullptr;
		stream.elements = 0;
	}
	for (auto& element : elements) {
		element.buffer = nullptr;
		element.num_components = 0;
		element.offset = 0;
	}
}

Mesh::~Mesh() {
	reset();
	std::cout << "Mesh @" << this << ": kthxbai";
}


////////////////////////////////////////////////////////////////////////////////
// lifecycle // GOOD

void Mesh::acquire() {
	refcount++;
}

void Mesh::release() {
	refcount--;
	if (refcount == 0) {
		delete this;
	}
}


////////////////////////////////////////////////////////////////////////////////
// load

// GOOD
void Mesh::load(const char* file_path) {
	size_t s = strlen(file_path);
	auto wstr = std::make_unique<wchar_t>(s);
	return load(wstr.get());
}

void Mesh::load(const wchar_t* file_path) {
	// TODO: implement this
}



////////////////////////////////////////////////////////////////////////////////
// new modify


// update whole mesh
bool Mesh::update(MeshData data) {
	reset();

	// a little utility for cleaning up half-baked mesh if an error occurs
	// no way I can track like 10 return statements
	struct ScopeGuardT {
		Mesh* parent;
		bool perform_cleanup;
		~ScopeGuardT() {
			if (perform_cleanup) {
				parent->reset();
			}
		}
	} scope_guard{this, true};


	// validate input elements, calculate internal elements
	unsigned element_flag = 0;
	bool is_position = false;
	bool is_tangent = false;
	bool is_normal = false;
	bool is_bitangent = false;
	num_elements = 0;

	for (int i = 0; i < data.vertex_elements_num; i++) {
		// check duplicate semantics
		if ((element_flag & (unsigned)data.vertex_elements[i].semantic) != 0) {
			return false; // error: duplicate semantic
		}
		element_flag |= data.vertex_elements[i].semantic;

		// check invalid num_components
		if ((data.vertex_elements[i].semantic == POSITION && data.vertex_elements[i].num_components != 3) ||
			(data.vertex_elements[i].semantic == NORMAL && data.vertex_elements[i].num_components != 3) ||
			(data.vertex_elements[i].semantic == TANGENT && data.vertex_elements[i].num_components != 3))
		{
			return false; // invalid component number
		}

		// monitor position and tangent
		if (data.vertex_elements[i].semantic == POSITION) {
			is_position = true;
		}
		else if (data.vertex_elements[i].semantic == TANGENT) {
			is_tangent = true;
		}
		else if (data.vertex_elements[i].semantic == NORMAL) {
			is_normal = true;
		}
		else if (data.vertex_elements[i].semantic == BITANGENT) {
			is_bitangent = true;
		}

		// add element to internals
		elements[i] = getBaseInfo(data.vertex_elements[i]);
		num_elements++;
	}

	if (is_bitangent ||
		(is_tangent && !is_normal) ||
		!is_position)
	{
		return false; // invalid input semantic combination
	}

	// sort internal elements
	if (is_tangent) {
		elements[data.vertex_elements_num] = getBaseInfo(ElementDesc{BITANGENT, 3});
		num_elements++;
	}
	std::sort(elements, elements + num_elements, [](const ElementInfo& o1, const ElementInfo& o2){ return o1.semantic < o2.semantic; });



	// calculate input stride, internal stride, vertex count
	int input_stride = getFormatStrideInput(data.vertex_elements, data.vertex_elements_num);
	int internal_stride = getFormatStrideInternal(elements, num_elements);
	size_t num_vertices = data.vertex_bytes / (size_t)input_stride;



	// validate data
	static size_t null_mat_id = 0;
	if (!data.mat_ids) {
		data.mat_ids = &null_mat_id;
		data.mat_ids_num = 1;
	}

	bool is_valid = validate(num_vertices, data.index_data, data.index_num, data.mat_ids, data.mat_ids_num);
	if (!is_valid) {
		return false; // out of bounds
	}



	// pack components
	void* packed_vertex_data = operator new(internal_stride * num_vertices);
	packVertices(data.vertex_elements, elements, data.vertex_elements_num,  num_elements, data.vertex_data, packed_vertex_data, num_vertices);



	// compute mat ids
	mat_ids.resize(data.mat_ids_num + 1);
	for (size_t i = 0; i < data.mat_ids_num; i++) {
		mat_ids[i] = data.mat_ids[i];
	}
	mat_ids[mat_ids.size() - 1] = data.index_num / 3;



	// optimize mesh
	optimize(packed_vertex_data, num_vertices, internal_stride, data.index_data, data.index_num, data.mat_ids, data.mat_ids_num);



	// split vertex data:
	// i'll be glad if it works with this hardcoded 1 stream version...
	// todo: make grouping policies

	////////////////////////////////////
	// WARNING: 
	//	vertex buffers are subject to change

	// create and fill vertex arrays
	rBuffer vb_desc;
	vb_desc.is_readable = false;
	vb_desc.is_writable = false;
	vb_desc.is_persistent = true;
	vb_desc.prefer_cpu_storage = false;
	vb_desc.size = internal_stride * num_vertices;

	rBuffer ib_desc;
	ib_desc.is_readable = false;
	ib_desc.is_writable = true;
	ib_desc.is_persistent = false;
	ib_desc.prefer_cpu_storage = false;
	ib_desc.size = data.index_num * sizeof(u32);

	IVertexBuffer* _vb = gapi->createVertexBuffer(vb_desc);
	IIndexBuffer* _ib = gapi->createIndexBuffer(ib_desc);

	_vb->update((char*)packed_vertex_data, vb_desc.size, 0);
	_ib->update((char*)data.index_data, ib_desc.size, 0);

	if (!_ib || !_vb) {
		cout << "Mesh @" << this << ": failed to create buffers" << endl;
		return false;
	}

	// END WARNING
	////////////////////////////////////

	streams[0].vb = _vb;
	num_streams = 1;
	ib = _ib;



	// set elements (vb and offset, as other params are set)
	int offset = 0;
	for (int i = 0; i < num_elements; i++) {
		streams[0].elements |= elements[i].semantic;
		elements[i].buffer = streams[0].vb;
		elements[i].offset = offset;
		offset += elements[i].num_components*elements[i].width / 8;
	}

	// cleanup
	operator delete(packed_vertex_data);

	scope_guard.perform_cleanup = false;
	return true;
}

// update vertex data
bool Mesh::updateVertexData(const void* data, size_t offset, size_t size) {
	std::cout << "[WARNING] updating vertex data does not work at the moment" << std::endl;
	return false;
}


// reset
//	- delete all underlying resources
void Mesh::reset() {
	for (auto& stream : streams) {
		if (stream.vb) {
			//stream.vb->destroy();
			stream.vb = nullptr;
			stream.elements = 0;
		}
	}
	for (auto& element : elements) {
		element.buffer = nullptr;
		element.num_components = 0;
		element.offset = 0;
	}
	if (ib) {
		//ib->destroy();
		ib = nullptr;
	}
	mat_ids.clear();

	num_elements = 0;
	num_streams = 0;
}


////////////////////////////////////////////////////////////////////////////////
// helper

// optimize data for gpu drawing
void Mesh::optimize(void* vertex_data, size_t num_verts, int vertex_stride,
	u32* index_data, size_t num_indices,
	size_t* mat_ids, size_t num_mat_ids)
{
	// TODO: implement
	return;
}


// validate data for out-of-bound cases
bool Mesh::validate(size_t num_verts,
	u32* index_data, size_t num_indices,
	size_t* mat_ids, size_t num_mat_ids)
{
	// criteria:
	// - indices num must be divisible by 3
	if (num_indices % 3 != 0) {
		return false;
	}

	// - indices must not over-index vertices
	for (size_t i = 0; i < num_indices; i++) {
		if (index_data[i] >= num_verts) {
			return false;
		}
	}

	// - mat ids must not over-index indices
	// - mat ids must be in order, equality allowed for empty groups
	size_t prev = 0;
	for (size_t i = 0; i < num_mat_ids; i++) {
		// mtl ids not in order (equality allowed, empty groups)
		if (mat_ids[i] < prev) {
			return false;
		}
		// overindex indices/faces
		if (mat_ids[i] >= num_indices / 3) {
			return false;
		}
		prev = mat_ids[i];
	}

	return true;
}


// pack vertices to smaller format
void PackPosition(void* input, void* output) {
	float* in = (float*)input;
	float* out = (float*)output;
	out[0] = in[0];
	out[1] = in[1];
	out[2] = in[2];
}

void PackNormal(void* input, void* output) {
	float x = ((float*)input)[0],
		y = ((float*)input)[1],
		z = ((float*)input)[2];
	u16* out = (u16*)output;
	out[0] = 65535.f*(0.5f + 0.5f*x);
	out[1] = 32767.5f*(0.5f + 0.5f*y) + (z > 0.0f) * 32767.5;
}

void PackTangent(void* input, void* output) {
	PackNormal(input, output);
}
void PackBitangent(void* input, void* output) {
	PackNormal(input, output);
}

void PackCopy(void* input, void* output, size_t bytes) { // copies 4x floats!
	memcpy(output, input, bytes);
}

void Mesh::packVertices(ElementDesc* input_format, ElementInfo* output_format, int input_count, int output_count, void* input, void* output, size_t num_verts) {
	// create a function that can pack 1 vertex

	// compute offsets by semantics, that is, where a semantic is found
	int offset_input[20]; // semantic's index is log2(semantic)
	int size_input[20];
	int offset_output[20];
	for (int i = 0; i < 20; i++) {
		offset_input[i] = -1;
		size_input[i] = 0;
		offset_output[i] = -1;
	}
	for (int i = 0, offset = 0; i < input_count; i++) {
		int size = input_format[i].num_components * sizeof(float);
		offset_input[fast_log2(input_format[i].semantic)] = offset;
		size_input[fast_log2(input_format[i].semantic)] = size;
		offset += size;
	}
	for (int i = 0, offset = 0; i < output_count; i++) {
		int size = output_format[i].num_components * output_format[i].width / 8;
		offset_output[fast_log2(output_format[i].semantic)] = offset;
		offset += size;
	}

	// enumerate output semantics
	std::vector<ElementSemantic> semantics;
	for (int i = 0; i < output_count; i++) {
		semantics.push_back(output_format[i].semantic);
	}

	// define a pack function
	auto PackVertex = [&offset_input, &offset_output, semantics](void* input, void* output) {
		vec3 normal, tangent;
		bool is_bitangent = false;

		// pack each semantic
		for (auto semantic : semantics) {
			int offseti = offset_input[fast_log2(semantic)];
			int offseto = offset_output[fast_log2(semantic)];
			void* off_in = (void*)(size_t(input) + offseti);
			void* off_out = (void*)(size_t(output) + offseto);

			if (semantic == POSITION) {
				PackPosition(off_in, off_out);
			}
			else if (semantic == NORMAL) {
				float* tmp = (float*)off_in;
				normal.x = tmp[0];
				normal.y = tmp[1];
				normal.z = tmp[2];

				PackNormal(off_in, off_out);
			}
			else if (semantic == TANGENT) {
				float* tmp = (float*)off_in;
				tangent.x = tmp[0];
				tangent.y = tmp[1];
				tangent.z = tmp[2];

				PackTangent(off_in, off_out);
			}
			else if (semantic == BITANGENT) {
				is_bitangent = true;
			}
			else if (COLOR0 <= semantic && semantic <= COLOR7) {

			}
			else if (TEX0 < semantic && semantic <= TEX7) {

			}
		}

		// compute and pack bitangent
		if (is_bitangent) {
			float tmp[3];
			vec3 bitangent = normalize(cross(normal, tangent));
			tmp[0] = bitangent.x;
			tmp[1] = bitangent.y;
			tmp[2] = bitangent.z;

			int offseto = offset_output[fast_log2(BITANGENT)];
			void* off_out = (void*)(size_t(output) + offseto);
			PackBitangent(tmp, off_out);
		}
	};


	// go through and pack all vertices
	size_t input_raw = (size_t)input;
	size_t output_raw = (size_t)output;
	size_t input_stride = getFormatStrideInput(input_format, input_count);
	size_t output_stride = getFormatStrideInternal(output_format, output_count);
	for (size_t vertex = 0; vertex < num_verts; vertex++) {
		PackVertex((void*)(input_raw + input_stride*vertex), (void*)(output_raw + output_stride*vertex));
	}
}


////////////////////////////////////////////////////////////////////////////////
// vertex format

bool Mesh::getElementBySemantic(ElementInfo& info, ElementSemantic semantic) const {
	for (int i = 0; i < num_elements; i++) {
		if (semantic == elements[i].semantic) {
			info = elements[i];
			return true;
		}
	}
	return false;
}

int Mesh::getElementsNum() const {
	return num_elements;
}

const Mesh::ElementInfo* Mesh::getElements() const {
	return elements;
}


// get a unique identifier corresponding to the mesh's layout
// the ID is 64 bits wide:
//	bits 0-18: streams (a 'composition' of N, where N={number of elements} )
//	bits 18-57: number of components for each element, 2 bit/element


// don't even try to understand it... http://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetParallel
static inline int NumBitsSet(u32 v) {
	v = v - ((v >> 1) & 0x55555555);
	v = (v & 0x33333333) + ((v >> 2) & 0x33333333);
	return ((v + (v >> 4) & 0xF0F0F0F) * 0x1010101) >> 24;
}

u64 Mesh::getElementConfigId() const {
	u64 id = 0;
	u32 comp = 0;

	// calculate composition
	// see http://en.wikipedia.org/wiki/Composition_(combinatorics) for nice drawing about binary representation
	for (int i = 0, j = 0; i < num_streams; i++) {
		assert(j < num_elements); // indicates the streams' elements are not set correctly
		int weight = NumBitsSet(streams[i].elements); // how many elements does that stream have?
		for (int k = 1; k < weight; k++) {
			comp |= (1u << j);
			j++;
		}
		j++;
	}

	// calculate num_components
	// so... there are 5 states: 0 to 4 components, 0 means does not exist
	// which, for 20 semantics, yields log2(5^20) = 46.43 bits
	// since position is implicit, log2(5^19) = 44.11 bits -> 45 bits
	// with the 19 bits of composition, it is exactly 64 bits - cool
	// we must write this stuff in base 5 number system
	assert((num_elements == 0) || (num_elements > 0 && elements[0].semantic == POSITION)); // no implicit position if an earlier flaw
	for (int i = 1, j = 0; i < num_elements; i++, j+=2) {
		id += elements[i].num_components;
		id *= 5;
	}
	assert(id >> 45 == 0); // that should not be... see above
	id <<= 19;
	id |= comp;

	return id;
}
