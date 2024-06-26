#define _NBL_STATIC_LIB_
#include <nabla.h>

#include "../common/CommonAPI.h"
#include "nbl/ext/FullScreenTriangle/FullScreenTriangle.h"
#include "glm/glm/glm.hpp"
#include <nbl/builtin/hlsl/cpp_compat/matrix.hlsl>
#include <nbl/builtin/hlsl/cpp_compat/vector.hlsl>
#include "curves.h"

static constexpr bool DebugMode = false;
static constexpr bool FragmentShaderPixelInterlock = true;

enum class ExampleMode
{
	CASE_0, // Simple Line, Camera Zoom In/Out
	CASE_1,	// Overdraw Fragment Shader Stress Test
	CASE_2, // NOT USED
	CASE_3, // CURVES AND LINES
};

constexpr ExampleMode mode = ExampleMode::CASE_3;
using namespace nbl::hlsl;

#include "common.hlsl"

static_assert(sizeof(DrawObject) == 16u);
static_assert(sizeof(MainObject) == 8u);
static_assert(sizeof(Globals) == 112u);
static_assert(sizeof(LineStyle) == 32u);
static_assert(sizeof(ClipProjectionData) == 88u);

using namespace nbl;
using namespace ui;

class Camera2D : public core::IReferenceCounted
{
public:
	Camera2D()
	{}

	void setOrigin(const float64_t2& origin)
	{
		m_origin = origin;
	}

	void setAspectRatio(const double& aspectRatio)
	{
		m_aspectRatio = aspectRatio;
	}

	void setSize(const double size)
	{
		m_bounds = float64_t2{ size * m_aspectRatio, size };
	}

	float64_t2 getBounds() const
	{
		return m_bounds;
	}

	float64_t3x3 constructViewProjection()
	{
		float64_t3x3 ret = {};

		ret[0][0] = 2.0 / m_bounds.x;
		ret[1][1] = -2.0 / m_bounds.y;
		ret[2][2] = 1.0;

		ret[0][2] = (-2.0 * m_origin.x) / m_bounds.x;
		ret[1][2] = (2.0 * m_origin.y) / m_bounds.y;

		return ret;
	}

	void mouseProcess(const nbl::ui::IMouseEventChannel::range_t& events)
	{
		for (auto eventIt = events.begin(); eventIt != events.end(); eventIt++)
		{
			auto ev = *eventIt;

			if (ev.type == nbl::ui::SMouseEvent::EET_SCROLL)
			{
				m_bounds = m_bounds + float64_t2{ (double)ev.scrollEvent.verticalScroll * -0.1 * m_aspectRatio, (double)ev.scrollEvent.verticalScroll * -0.1};
				m_bounds = float64_t2{ core::max(m_aspectRatio, m_bounds.x), core::max(1.0, m_bounds.y) };
			}
		}
	}

	void keyboardProcess(const IKeyboardEventChannel::range_t& events)
	{
		for (auto eventIt = events.begin(); eventIt != events.end(); eventIt++)
		{
			auto ev = *eventIt;

			if (ev.action == nbl::ui::SKeyboardEvent::E_KEY_ACTION::ECA_PRESSED && ev.keyCode == nbl::ui::E_KEY_CODE::EKC_W)
			{
				m_origin.y += 1;
			}
			if (ev.action == nbl::ui::SKeyboardEvent::E_KEY_ACTION::ECA_PRESSED && ev.keyCode == nbl::ui::E_KEY_CODE::EKC_A)
			{
				m_origin.x -= 1;
			}
			if (ev.action == nbl::ui::SKeyboardEvent::E_KEY_ACTION::ECA_PRESSED && ev.keyCode == nbl::ui::E_KEY_CODE::EKC_S)
			{
				m_origin.y -= 1;
			}
			if (ev.action == nbl::ui::SKeyboardEvent::E_KEY_ACTION::ECA_PRESSED && ev.keyCode == nbl::ui::E_KEY_CODE::EKC_D)
			{
				m_origin.x += 1;
			}
		}
	}
private:

	double m_aspectRatio = 0.0;
	float64_t2 m_bounds = {};
	float64_t2 m_origin = {};
};

// It is not optimized because how you feed a Polyline to our cad renderer is your choice. this is just for convenience
// This is a Nabla Polyline used to feed to our CAD renderer. You can convert your Polyline to this class. or just use it directly.
class CPolyline
{
public:

	// each section consists of multiple connected lines or multiple connected ellipses
	struct SectionInfo
	{
		ObjectType	type;
		uint32_t	index; // can't make this a void* cause of vector resize
		uint32_t	count;
	};

	struct EllipticalArcInfo
	{
		float64_t2 majorAxis;
		float64_t2 center;
		float64_t2 angleBounds; // [0, 2Pi)
		double eccentricity; // (0, 1]

		bool isValid() const
		{
			if (eccentricity > 1.0 || eccentricity < 0.0)
				return false;
			if (angleBounds.y < angleBounds.x)
				return false;
			if ((angleBounds.y - angleBounds.x) > 2 * core::PI<double>())
				return false;
			return true;
		}
	};

	size_t getSectionsCount() const { return m_sections.size(); }

	const SectionInfo& getSectionInfoAt(const uint32_t idx) const
	{
		return m_sections[idx];
	}

	const QuadraticBezierInfo& getQuadBezierInfoAt(const uint32_t idx) const
	{
		return m_quadBeziers[idx];
	}

	const float64_t2& getLinePointAt(const uint32_t idx) const
	{
		return m_linePoints[idx];
	}

	void clearEverything()
	{
		m_sections.clear();
		m_linePoints.clear();
	}

	// Reserves memory with worst case
	void reserveMemory(uint32_t noOfLines, uint32_t noOfBeziers)
	{
		m_sections.reserve(noOfLines + noOfBeziers);
		m_linePoints.reserve(noOfLines * 2u);
		m_quadBeziers.reserve(noOfBeziers);
	}

	void addLinePoints(const core::SRange<float64_t2>& linePoints)
	{
		if (linePoints.size() <= 1u)
			return;

		bool addNewSection = m_sections.size() == 0u || m_sections[m_sections.size() - 1u].type != ObjectType::LINE;
		if (addNewSection)
		{
			SectionInfo newSection = {};
			newSection.type = ObjectType::LINE;
			newSection.index = m_linePoints.size();
			newSection.count = linePoints.size() - 1u;
			m_sections.push_back(newSection);
		}
		else
		{
			m_sections[m_sections.size() - 1u].count += linePoints.size();
		}
		m_linePoints.insert(m_linePoints.end(), linePoints.begin(), linePoints.end());
	}

	void addEllipticalArcs(const core::SRange<EllipticalArcInfo>& ellipses)
	{
		// TODO[Erfan] Approximate with quadratic beziers
	}

	// TODO[Przemek]: This uses the struct from the shader common.hlsl if you need to precompute stuff make a duplicate of this struct here first (for the user input to fill)
	// and then do the precomputation here and store in m_quadBeziers which holds the actual structs that will be fed to the GPU
	void addQuadBeziers(const core::SRange<QuadraticBezierInfo>& quadBeziers)
	{
		bool addNewSection = m_sections.size() == 0u || m_sections[m_sections.size() - 1u].type != ObjectType::QUAD_BEZIER;
		if (addNewSection)
		{
			SectionInfo newSection = {};
			newSection.type = ObjectType::QUAD_BEZIER;
			newSection.index = m_quadBeziers.size();
			newSection.count = quadBeziers.size();
			m_sections.push_back(newSection);
		}
		else
		{
			m_sections[m_sections.size() - 1u].count += quadBeziers.size();
		}
		m_quadBeziers.insert(m_quadBeziers.end(), quadBeziers.begin(), quadBeziers.end());
	}

protected:
	std::vector<SectionInfo> m_sections;
	std::vector<float64_t2> m_linePoints;
	std::vector<QuadraticBezierInfo> m_quadBeziers;
};

// Basically 2D CSG
// TODO[Lucas]:
class Hatch
{
	/*
		This class will input a list of Polylines (core::SRange)
		and then output bunch of HatchBoxes
		The hatch box generation algorithm will be used here
	*/

	/*
		Here are additional info you need for the hatch box generation algorithm:

		1. Curve-Curve Intersection
			For curve curve intersection you'd need one curve's implicit formula F(x,y)=0 and another ones parametric formula x=x(t) and y=y(t)
			we substitude x and y in F(x,y) with x(t) and y(t) and that results in a polynomial F(x(t),y(t))=g(t)
			whose roots are the parameter values of the points of intersection
			for more info See Chapter 17.8 of https://scholarsarchive.byu.edu/cgi/viewcontent.cgi?article=1000&context=facpub
			For quadratic beziers the equation will be quartic (degree 4 of t). solve the quartic using the method here https://github.com/erich666/GraphicsGems/blob/master/gems/Roots3And4.c

		2. Implicitization
			See Chapter 17.6 of https://scholarsarchive.byu.edu/cgi/viewcontent.cgi?article=1000&context=facpub
			You need to implicitize the quadratic bezier curve which results in a polynomial like this: ax^2+by^2+cxy+dx+ey+f.
			that's beautiful cause all you need to store this is 6 doubles just like a quadratic bezier,
			you could even standardize and divide every component by 'a' and use 5 doubles, but let's not do that yet, I'm a bit scared of divisions and haven't thought about this fully
			We need a implicitized curve per curve (1 to 1 mapping) in our algorithm, we don't need to store these in the Hatch class
			And here is my desmos showing the implicitization process https://www.desmos.com/calculator/8jfbzqrazh

		3. for the segment sorting you also need to evaluate derivatives in the case that multiple beziers go through the same point
			(Talk with Matt, he figure out the math)
	*/

	// this struct will be filled in cpu and sent to gpu for processing as a single DrawObj
	struct CurveBox
	{
		// aabb (float64_t2 min, max)
		// reference to min curve (could be left curve if sweeping in y dir) and it's tmin tmax
		// reference to max curve (could be right curve if sweeping in y dir) and it's tmin tmax
		// any reference to texture or colour or style used to fill it Or we could fill that in It's drawObj (latter is better if we could alias with lineStyle address)
	};

	// note: even though in this example we can reference to the polyline bezier and lines somehow, we want to eventualy be able to serialize/deserialize 
	// this object and should be independant of any outside references so here we will also be keeping a list/vector of quadratic beziers which CurveBox can index into
	// we have two different types (line,bezier) but we don't want to keep two seperate lists, we will have the lines have the mid point (p1) set to nan adn everything as "beziers"
};

template <typename BufferType>
struct DrawBuffers
{
	core::smart_refctd_ptr<BufferType> indexBuffer;
	core::smart_refctd_ptr<BufferType> mainObjectsBuffer;
	core::smart_refctd_ptr<BufferType> drawObjectsBuffer;
	core::smart_refctd_ptr<BufferType> geometryBuffer;
	core::smart_refctd_ptr<BufferType> lineStylesBuffer;
	core::smart_refctd_ptr<BufferType> customClipProjectionBuffer;
};

// ! this is just a buffers filler with autosubmission features used for convenience to how you feed our CAD renderer
struct DrawBuffersFiller
{
public:

	typedef uint32_t index_buffer_type;

	DrawBuffersFiller() {}

	DrawBuffersFiller(core::smart_refctd_ptr<nbl::video::IUtilities>&& utils)
	{
		utilities = utils;
	}

	typedef std::function<video::IGPUQueue::SSubmitInfo(video::IGPUQueue*, video::IGPUFence*, video::IGPUQueue::SSubmitInfo)> SubmitFunc;

	// function is called when buffer is filled and we should submit draws and clear the buffers and continue filling
	void setSubmitDrawsFunction(SubmitFunc func)
	{
		submitDraws = func;
	}

	void allocateIndexBuffer(core::smart_refctd_ptr<nbl::video::ILogicalDevice> logicalDevice, uint32_t indices)
	{
		maxIndices = indices;
		const size_t indexBufferSize = maxIndices * sizeof(uint32_t);

		video::IGPUBuffer::SCreationParams indexBufferCreationParams = {};
		indexBufferCreationParams.size = indexBufferSize;
		indexBufferCreationParams.usage = video::IGPUBuffer::EUF_INDEX_BUFFER_BIT | video::IGPUBuffer::EUF_TRANSFER_DST_BIT;
		gpuDrawBuffers.indexBuffer = logicalDevice->createBuffer(std::move(indexBufferCreationParams));
		gpuDrawBuffers.indexBuffer->setObjectDebugName("indexBuffer");

		video::IDeviceMemoryBacked::SDeviceMemoryRequirements memReq = gpuDrawBuffers.indexBuffer->getMemoryReqs();
		memReq.memoryTypeBits &= logicalDevice->getPhysicalDevice()->getDeviceLocalMemoryTypeBits();
		auto indexBufferMem = logicalDevice->allocate(memReq, gpuDrawBuffers.indexBuffer.get());

		cpuDrawBuffers.indexBuffer = core::make_smart_refctd_ptr<asset::ICPUBuffer>(indexBufferSize);
	}

	void allocateMainObjectsBuffer(core::smart_refctd_ptr<nbl::video::ILogicalDevice> logicalDevice, uint32_t mainObjects)
	{
		maxMainObjects = mainObjects;
		size_t mainObjectsBufferSize = mainObjects * sizeof(MainObject);

		video::IGPUBuffer::SCreationParams mainObjectsCreationParams = {};
		mainObjectsCreationParams.size = mainObjectsBufferSize;
		mainObjectsCreationParams.usage = video::IGPUBuffer::EUF_STORAGE_BUFFER_BIT | video::IGPUBuffer::EUF_TRANSFER_DST_BIT;
		gpuDrawBuffers.mainObjectsBuffer = logicalDevice->createBuffer(std::move(mainObjectsCreationParams));
		gpuDrawBuffers.mainObjectsBuffer->setObjectDebugName("mainObjectsBuffer");

		video::IDeviceMemoryBacked::SDeviceMemoryRequirements memReq = gpuDrawBuffers.mainObjectsBuffer->getMemoryReqs();
		memReq.memoryTypeBits &= logicalDevice->getPhysicalDevice()->getDeviceLocalMemoryTypeBits();
		auto mainObjectsBufferMem = logicalDevice->allocate(memReq, gpuDrawBuffers.mainObjectsBuffer.get());

		cpuDrawBuffers.mainObjectsBuffer = core::make_smart_refctd_ptr<asset::ICPUBuffer>(mainObjectsBufferSize);
	}

	void allocateDrawObjectsBuffer(core::smart_refctd_ptr<nbl::video::ILogicalDevice> logicalDevice, uint32_t drawObjects)
	{
		maxDrawObjects = drawObjects;
		size_t drawObjectsBufferSize = drawObjects * sizeof(DrawObject);

		video::IGPUBuffer::SCreationParams drawObjectsCreationParams = {};
		drawObjectsCreationParams.size = drawObjectsBufferSize;
		drawObjectsCreationParams.usage = video::IGPUBuffer::EUF_STORAGE_BUFFER_BIT | video::IGPUBuffer::EUF_TRANSFER_DST_BIT;
		gpuDrawBuffers.drawObjectsBuffer = logicalDevice->createBuffer(std::move(drawObjectsCreationParams));
		gpuDrawBuffers.drawObjectsBuffer->setObjectDebugName("drawObjectsBuffer");

		video::IDeviceMemoryBacked::SDeviceMemoryRequirements memReq = gpuDrawBuffers.drawObjectsBuffer->getMemoryReqs();
		memReq.memoryTypeBits &= logicalDevice->getPhysicalDevice()->getDeviceLocalMemoryTypeBits();
		auto drawObjectsBufferMem = logicalDevice->allocate(memReq, gpuDrawBuffers.drawObjectsBuffer.get());

		cpuDrawBuffers.drawObjectsBuffer = core::make_smart_refctd_ptr<asset::ICPUBuffer>(drawObjectsBufferSize);
	}

	void allocateGeometryBuffer(core::smart_refctd_ptr<nbl::video::ILogicalDevice> logicalDevice, size_t size)
	{
		maxGeometryBufferSize = size;

		video::IGPUBuffer::SCreationParams geometryCreationParams = {};
		geometryCreationParams.size = size;
		geometryCreationParams.usage = core::bitflag(video::IGPUBuffer::EUF_STORAGE_BUFFER_BIT) | video::IGPUBuffer::EUF_SHADER_DEVICE_ADDRESS_BIT | video::IGPUBuffer::EUF_TRANSFER_DST_BIT;
		gpuDrawBuffers.geometryBuffer = logicalDevice->createBuffer(std::move(geometryCreationParams));
		gpuDrawBuffers.geometryBuffer->setObjectDebugName("geometryBuffer");

		video::IDeviceMemoryBacked::SDeviceMemoryRequirements memReq = gpuDrawBuffers.geometryBuffer->getMemoryReqs();
		memReq.memoryTypeBits &= logicalDevice->getPhysicalDevice()->getDeviceLocalMemoryTypeBits();
		auto geometryBufferMem = logicalDevice->allocate(memReq, gpuDrawBuffers.geometryBuffer.get(), video::IDeviceMemoryAllocation::EMAF_DEVICE_ADDRESS_BIT);
		geometryBufferAddress = logicalDevice->getBufferDeviceAddress(gpuDrawBuffers.geometryBuffer.get());

		cpuDrawBuffers.geometryBuffer = core::make_smart_refctd_ptr<asset::ICPUBuffer>(size);
	}

	void allocateStylesBuffer(core::smart_refctd_ptr<nbl::video::ILogicalDevice> logicalDevice, uint32_t stylesCount)
	{
		maxLineStyles = stylesCount;
		size_t lineStylesBufferSize = stylesCount * sizeof(LineStyle);

		video::IGPUBuffer::SCreationParams lineStylesCreationParams = {};
		lineStylesCreationParams.size = lineStylesBufferSize;
		lineStylesCreationParams.usage = video::IGPUBuffer::EUF_STORAGE_BUFFER_BIT | video::IGPUBuffer::EUF_TRANSFER_DST_BIT;
		gpuDrawBuffers.lineStylesBuffer = logicalDevice->createBuffer(std::move(lineStylesCreationParams));
		gpuDrawBuffers.lineStylesBuffer->setObjectDebugName("lineStylesBuffer");

		video::IDeviceMemoryBacked::SDeviceMemoryRequirements memReq = gpuDrawBuffers.lineStylesBuffer->getMemoryReqs();
		memReq.memoryTypeBits &= logicalDevice->getPhysicalDevice()->getDeviceLocalMemoryTypeBits();
		auto stylesBufferMem = logicalDevice->allocate(memReq, gpuDrawBuffers.lineStylesBuffer.get());

		cpuDrawBuffers.lineStylesBuffer = core::make_smart_refctd_ptr<asset::ICPUBuffer>(lineStylesBufferSize);
	}

	void allocateCustomClipProjectionBuffer(core::smart_refctd_ptr<nbl::video::ILogicalDevice> logicalDevice, uint32_t ClipProjectionDataCount)
	{
		maxClipProjectionData = ClipProjectionDataCount;
		size_t customClipProjectionBufferSize = maxClipProjectionData * sizeof(ClipProjectionData);

		video::IGPUBuffer::SCreationParams customClipProjectionCreationParams = {};
		customClipProjectionCreationParams.size = customClipProjectionBufferSize;
		customClipProjectionCreationParams.usage = video::IGPUBuffer::EUF_STORAGE_BUFFER_BIT | video::IGPUBuffer::EUF_TRANSFER_DST_BIT;
		gpuDrawBuffers.customClipProjectionBuffer = logicalDevice->createBuffer(std::move(customClipProjectionCreationParams));
		gpuDrawBuffers.customClipProjectionBuffer->setObjectDebugName("customClipProjectionBuffer");

		video::IDeviceMemoryBacked::SDeviceMemoryRequirements memReq = gpuDrawBuffers.customClipProjectionBuffer->getMemoryReqs();
		memReq.memoryTypeBits &= logicalDevice->getPhysicalDevice()->getDeviceLocalMemoryTypeBits();
		auto customClipProjectionBufferMem = logicalDevice->allocate(memReq, gpuDrawBuffers.customClipProjectionBuffer.get());

		cpuDrawBuffers.customClipProjectionBuffer = core::make_smart_refctd_ptr<asset::ICPUBuffer>(customClipProjectionBufferSize);
	}

	uint32_t getIndexCount() const { return currentIndexCount; }
	
	// TODO[Przemek]: look at the `drawPolyline` function and you may have to change that as well. if you found out the user input `const LineStyle& lineStyle` for stippling needs processing/computation to be ready to be fed into gpu
	//	 then have two sturcts (one for cpu side and one that is private to this class and will be fed to gpu)
	//	 don't force yourself to understand this function completely, it will change soon when I change CSG algo, 
	//	 just be aware that this drawPolyline function will result in calls to addQuadBeziers_Internal and addLineStyle_Internal and will submit draws if there is no memory left and continue where it left off
	//! this function fills buffers required for drawing a polyline and submits a draw through provided callback when there is not enough memory.
	video::IGPUQueue::SSubmitInfo drawPolyline(
		const CPolyline& polyline,
		const LineStyle& lineStyle,
		const uint32_t clipProjectionIdx,
		video::IGPUQueue* submissionQueue,
		video::IGPUFence* submissionFence,
		video::IGPUQueue::SSubmitInfo intendedNextSubmit)
	{
		uint32_t styleIdx;
		intendedNextSubmit = addLineStyle_SubmitIfNeeded(lineStyle, styleIdx, submissionQueue, submissionFence, intendedNextSubmit);
		
		MainObject mainObj = {};
		mainObj.styleIdx = styleIdx;
		mainObj.clipProjectionIdx = clipProjectionIdx;
		uint32_t mainObjIdx;
		intendedNextSubmit = addMainObject_SubmitIfNeeded(mainObj, mainObjIdx, submissionQueue, submissionFence, intendedNextSubmit);

		const auto sectionsCount = polyline.getSectionsCount();

		uint32_t currentSectionIdx = 0u;
		uint32_t currentObjectInSection = 0u; // Object here refers to DrawObject used in vertex shader. You can think of it as a Cage.

		while (currentSectionIdx < sectionsCount)
		{
			bool shouldSubmit = false;
			const auto& currentSection = polyline.getSectionInfoAt(currentSectionIdx);
			addPolylineObjects_Internal(polyline, currentSection, currentObjectInSection, mainObjIdx);

			if (currentObjectInSection >= currentSection.count)
			{
				currentSectionIdx++;
				currentObjectInSection = 0u;
			}
			else
				shouldSubmit = true;

			if (shouldSubmit)
			{
				intendedNextSubmit = finalizeAllCopiesToGPU(submissionQueue, submissionFence, intendedNextSubmit);
				intendedNextSubmit = submitDraws(submissionQueue, submissionFence, intendedNextSubmit);
				resetIndexCounters();
				resetGeometryCounters();
				// We don't reset counters for linestyles, mainObjects and customClipProjection because we will be reusing them
				shouldSubmit = false;
			}
		}

		return intendedNextSubmit;
	}

	// TODO[Lucas]: drawHatch function with similar signature to drawPolyline
	// If we had infinite mem, we would first upload all curves into geometry buffer then upload the "CurveBoxes" with correct gpu addresses to those
	// But we don't have that so we have to follow a similar auto submission as the "drawPolyline" function with some mutations:
	// We have to find the MAX number of "CurveBoxes" we could draw, and since both the "Curves" and "CurveBoxes" reside in geometry buffer,
	// it has to be taken into account when calculating "how many curve boxes we could draw and when we need to submit/clear"
	// So same as drawPolylines, we would first try to fill the geometry buffer and index buffer that corresponds to "backfaces or even provoking vertices"
	// then change index buffer to draw front faces of the curveBoxes that already reside in geometry buffer memory
	// then if anything was left (the ones that weren't in memory for front face of the curveBoxes) we copy their geom to mem again and use frontface/oddProvoking vertex

	video::IGPUQueue::SSubmitInfo finalizeAllCopiesToGPU(
		video::IGPUQueue* submissionQueue,
		video::IGPUFence* submissionFence,
		video::IGPUQueue::SSubmitInfo intendedNextSubmit)
	{
		intendedNextSubmit = finalizeIndexCopiesToGPU(submissionQueue, submissionFence, intendedNextSubmit);
		intendedNextSubmit = finalizeMainObjectCopiesToGPU(submissionQueue, submissionFence, intendedNextSubmit);
		intendedNextSubmit = finalizeGeometryCopiesToGPU(submissionQueue, submissionFence, intendedNextSubmit);
		intendedNextSubmit = finalizeLineStyleCopiesToGPU(submissionQueue, submissionFence, intendedNextSubmit);
		intendedNextSubmit = finalizeCustomClipProjectionCopiesToGPU(submissionQueue, submissionFence, intendedNextSubmit);

		return intendedNextSubmit;
	}

	size_t getCurrentIndexBufferSize() const
	{
		return sizeof(index_buffer_type) * currentIndexCount;
	}

	size_t getCurrentMainObjectsBufferSize() const
	{
		return sizeof(MainObject) * currentMainObjectCount;
	}

	size_t getCurrentDrawObjectsBufferSize() const
	{
		return sizeof(DrawObject) * currentDrawObjectCount;
	}

	size_t getCurrentGeometryBufferSize() const
	{
		return currentGeometryBufferSize;
	}

	size_t getCurrentLineStylesBufferSize() const
	{
		return sizeof(LineStyle) * currentLineStylesCount;
	}

	size_t getCurrentCustomClipProjectionBufferSize() const
	{
		return sizeof(ClipProjectionData) * currentClipProjectionDataCount;
	}

	void reset()
	{
		resetAllCounters();
	}

	DrawBuffers<asset::ICPUBuffer> cpuDrawBuffers;
	DrawBuffers<video::IGPUBuffer> gpuDrawBuffers;

protected:

	SubmitFunc submitDraws;
	static constexpr uint32_t InvalidLineStyleIdx = ~0u;

	video::IGPUQueue::SSubmitInfo finalizeIndexCopiesToGPU(
		video::IGPUQueue* submissionQueue,
		video::IGPUFence* submissionFence,
		video::IGPUQueue::SSubmitInfo intendedNextSubmit)
	{
		// Copy Indices
		uint32_t remainingIndexCount = currentIndexCount - inMemIndexCount;
		asset::SBufferRange<video::IGPUBuffer> indicesRange = { sizeof(index_buffer_type) * inMemIndexCount, sizeof(index_buffer_type) * remainingIndexCount, gpuDrawBuffers.indexBuffer };
		const index_buffer_type* srcIndexData = reinterpret_cast<index_buffer_type*>(cpuDrawBuffers.indexBuffer->getPointer()) + inMemIndexCount;
		if (indicesRange.size > 0u)
			intendedNextSubmit = utilities->updateBufferRangeViaStagingBuffer(indicesRange, srcIndexData, submissionQueue, submissionFence, intendedNextSubmit);
		inMemIndexCount = currentIndexCount;
		return intendedNextSubmit;
	}

	video::IGPUQueue::SSubmitInfo finalizeMainObjectCopiesToGPU(
		video::IGPUQueue* submissionQueue,
		video::IGPUFence* submissionFence,
		video::IGPUQueue::SSubmitInfo intendedNextSubmit)
	{
		// Copy MainObjects
		uint32_t remainingMainObjects = currentMainObjectCount - inMemMainObjectCount;
		asset::SBufferRange<video::IGPUBuffer> mainObjectsRange = { sizeof(MainObject) * inMemMainObjectCount, sizeof(MainObject) * remainingMainObjects, gpuDrawBuffers.mainObjectsBuffer };
		const MainObject* srcMainObjData = reinterpret_cast<MainObject*>(cpuDrawBuffers.mainObjectsBuffer->getPointer()) + inMemMainObjectCount;
		if (mainObjectsRange.size > 0u)
			intendedNextSubmit = utilities->updateBufferRangeViaStagingBuffer(mainObjectsRange, srcMainObjData, submissionQueue, submissionFence, intendedNextSubmit);
		inMemMainObjectCount = currentMainObjectCount;
		return intendedNextSubmit;
	}

	video::IGPUQueue::SSubmitInfo finalizeGeometryCopiesToGPU(
		video::IGPUQueue* submissionQueue,
		video::IGPUFence* submissionFence,
		video::IGPUQueue::SSubmitInfo intendedNextSubmit)
	{
		// Copy DrawObjects
		uint32_t remainingDrawObjects = currentDrawObjectCount - inMemDrawObjectCount;
		asset::SBufferRange<video::IGPUBuffer> drawObjectsRange = { sizeof(DrawObject) * inMemDrawObjectCount, sizeof(DrawObject) * remainingDrawObjects, gpuDrawBuffers.drawObjectsBuffer };
		const DrawObject* srcDrawObjData = reinterpret_cast<DrawObject*>(cpuDrawBuffers.drawObjectsBuffer->getPointer()) + inMemDrawObjectCount;
		if (drawObjectsRange.size > 0u)
			intendedNextSubmit = utilities->updateBufferRangeViaStagingBuffer(drawObjectsRange, srcDrawObjData, submissionQueue, submissionFence, intendedNextSubmit);
		inMemDrawObjectCount = currentDrawObjectCount;

		// Copy GeometryBuffer
		uint32_t remainingGeometrySize = currentGeometryBufferSize - inMemGeometryBufferSize;
		asset::SBufferRange<video::IGPUBuffer> geomRange = { inMemGeometryBufferSize, remainingGeometrySize, gpuDrawBuffers.geometryBuffer };
		const uint8_t* srcGeomData = reinterpret_cast<uint8_t*>(cpuDrawBuffers.geometryBuffer->getPointer()) + inMemGeometryBufferSize;
		if (geomRange.size > 0u)
			intendedNextSubmit = utilities->updateBufferRangeViaStagingBuffer(geomRange, srcGeomData, submissionQueue, submissionFence, intendedNextSubmit);
		inMemGeometryBufferSize = currentGeometryBufferSize;

		return intendedNextSubmit;
	}

	video::IGPUQueue::SSubmitInfo finalizeLineStyleCopiesToGPU(
		video::IGPUQueue* submissionQueue,
		video::IGPUFence* submissionFence,
		video::IGPUQueue::SSubmitInfo intendedNextSubmit)
	{
		// Copy LineStyles
		uint32_t remainingLineStyles = currentLineStylesCount - inMemLineStylesCount;
		asset::SBufferRange<video::IGPUBuffer> stylesRange = { sizeof(LineStyle) * inMemLineStylesCount, sizeof(LineStyle) * remainingLineStyles, gpuDrawBuffers.lineStylesBuffer };
		const LineStyle* srcLineStylesData = reinterpret_cast<LineStyle*>(cpuDrawBuffers.lineStylesBuffer->getPointer()) + inMemLineStylesCount;
		if (stylesRange.size > 0u)
			intendedNextSubmit = utilities->updateBufferRangeViaStagingBuffer(stylesRange, srcLineStylesData, submissionQueue, submissionFence, intendedNextSubmit);
		inMemLineStylesCount = currentLineStylesCount;
		return intendedNextSubmit;
	}

	video::IGPUQueue::SSubmitInfo finalizeCustomClipProjectionCopiesToGPU(
		video::IGPUQueue* submissionQueue,
		video::IGPUFence* submissionFence,
		video::IGPUQueue::SSubmitInfo intendedNextSubmit)
	{
		// Copy LineStyles
		uint32_t remainingClipProjectionData = currentClipProjectionDataCount - inMemClipProjectionDataCount;
		asset::SBufferRange<video::IGPUBuffer> clipProjectionRange = { sizeof(ClipProjectionData) * inMemClipProjectionDataCount, sizeof(ClipProjectionData) * remainingClipProjectionData, gpuDrawBuffers.customClipProjectionBuffer };
		const ClipProjectionData* srcClipProjectionData = reinterpret_cast<ClipProjectionData*>(cpuDrawBuffers.customClipProjectionBuffer->getPointer()) + inMemClipProjectionDataCount;
		if (clipProjectionRange.size > 0u)
			intendedNextSubmit = utilities->updateBufferRangeViaStagingBuffer(clipProjectionRange, srcClipProjectionData, submissionQueue, submissionFence, intendedNextSubmit);
		inMemClipProjectionDataCount = currentClipProjectionDataCount;
		return intendedNextSubmit;
	}

	video::IGPUQueue::SSubmitInfo addMainObject_SubmitIfNeeded(
		const MainObject& mainObject,
		uint32_t& outMainObjectIdx,
		video::IGPUQueue* submissionQueue,
		video::IGPUFence* submissionFence,
		video::IGPUQueue::SSubmitInfo intendedNextSubmit)
	{
		outMainObjectIdx = addMainObject_Internal(mainObject);
		if (outMainObjectIdx == InvalidMainObjectIdx)
		{
			intendedNextSubmit = finalizeAllCopiesToGPU(submissionQueue, submissionFence, intendedNextSubmit);
			intendedNextSubmit = submitDraws(submissionQueue, submissionFence, intendedNextSubmit);
			resetAllCounters();
			outMainObjectIdx = addMainObject_Internal(mainObject);
			assert(outMainObjectIdx != InvalidMainObjectIdx);
		}
		return intendedNextSubmit;
	}

	uint32_t addMainObject_Internal(const MainObject& mainObject)
	{
		MainObject* mainObjsArray = reinterpret_cast<MainObject*>(cpuDrawBuffers.mainObjectsBuffer->getPointer());
		if (currentMainObjectCount >= maxMainObjects)
			return InvalidMainObjectIdx;

		void* dst = mainObjsArray + currentMainObjectCount;
		memcpy(dst, &mainObject, sizeof(MainObject));
		uint32_t ret = (currentMainObjectCount % MaxIndexableMainObjects); // just to wrap around if it ever exceeded (we pack this id into 24 bits)
		currentMainObjectCount++;
		return ret;
	}

	video::IGPUQueue::SSubmitInfo addLineStyle_SubmitIfNeeded(
		const LineStyle& lineStyle,
		uint32_t& outLineStyleIdx,
		video::IGPUQueue* submissionQueue,
		video::IGPUFence* submissionFence,
		video::IGPUQueue::SSubmitInfo intendedNextSubmit)
	{
		outLineStyleIdx = addLineStyle_Internal(lineStyle);
		if (outLineStyleIdx == InvalidLineStyleIdx)
		{
			intendedNextSubmit = finalizeAllCopiesToGPU(submissionQueue, submissionFence, intendedNextSubmit);
			intendedNextSubmit = submitDraws(submissionQueue, submissionFence, intendedNextSubmit);
			resetAllCounters();
			outLineStyleIdx = addLineStyle_Internal(lineStyle);
			assert(outLineStyleIdx != InvalidLineStyleIdx);
		}
		return intendedNextSubmit;
	}

	uint32_t addLineStyle_Internal(const LineStyle& lineStyle)
	{
		// TODO[Przemek]: styles are added here, store info about stipple patterns here, assume max input of the stipple array is 15 max (the -1, +2, 0., +4, .. patterns)
		LineStyle* stylesArray = reinterpret_cast<LineStyle*>(cpuDrawBuffers.lineStylesBuffer->getPointer());
		for (uint32_t i = 0u; i < currentLineStylesCount; ++i)
		{
			const LineStyle& itr = stylesArray[i];
			if (lineStyle.screenSpaceLineWidth == itr.screenSpaceLineWidth)
				if (lineStyle.worldSpaceLineWidth == itr.worldSpaceLineWidth)
					if (lineStyle.color == itr.color)
						return i;
		}

		if (currentLineStylesCount >= maxLineStyles)
			return InvalidLineStyleIdx;

		void* dst = stylesArray + currentLineStylesCount;
		memcpy(dst, &lineStyle, sizeof(LineStyle));
		return currentLineStylesCount++;
	}

public:
	video::IGPUQueue::SSubmitInfo addClipProjectionData_SubmitIfNeeded(
		const ClipProjectionData& clipProjectionData,
		uint32_t& outClipProjectionIdx,
		video::IGPUQueue* submissionQueue,
		video::IGPUFence* submissionFence,
		video::IGPUQueue::SSubmitInfo intendedNextSubmit)
	{
		outClipProjectionIdx = addClipProjectionData_Internal(clipProjectionData);
		if (outClipProjectionIdx == InvalidClipProjectionIdx)
		{
			intendedNextSubmit = finalizeAllCopiesToGPU(submissionQueue, submissionFence, intendedNextSubmit);
			intendedNextSubmit = submitDraws(submissionQueue, submissionFence, intendedNextSubmit);
			resetAllCounters();
			outClipProjectionIdx = addClipProjectionData_Internal(clipProjectionData);
			assert(outClipProjectionIdx != InvalidClipProjectionIdx);
		}
		return intendedNextSubmit;
	}

protected:
	uint32_t addClipProjectionData_Internal(const ClipProjectionData& clipProjectionData)
	{
		ClipProjectionData* clipProjectionArray = reinterpret_cast<ClipProjectionData*>(cpuDrawBuffers.customClipProjectionBuffer->getPointer());
		if (currentClipProjectionDataCount >= maxClipProjectionData)
			return InvalidClipProjectionIdx;

		void* dst = clipProjectionArray + currentClipProjectionDataCount;
		memcpy(dst, &clipProjectionData, sizeof(ClipProjectionData));
		return currentClipProjectionDataCount++;
	}

	static constexpr uint32_t getCageCountPerPolylineObject(ObjectType type)
	{
		if (type == ObjectType::LINE)
			return 1u;
		else if (type == ObjectType::QUAD_BEZIER)
			return 3u;
		return 0u;
	};

	void addPolylineObjects_Internal(const CPolyline& polyline, const CPolyline::SectionInfo& section, uint32_t& currentObjectInSection, uint32_t mainObjIdx)
	{
		if (section.type == ObjectType::LINE)
			addLines_Internal(polyline, section, currentObjectInSection, mainObjIdx);
		else if (section.type == ObjectType::QUAD_BEZIER)
			addQuadBeziers_Internal(polyline, section, currentObjectInSection, mainObjIdx);
		else
			assert(false); // we don't handle other object types
	}

	void addLines_Internal(const CPolyline& polyline, const CPolyline::SectionInfo& section, uint32_t& currentObjectInSection, uint32_t mainObjIdx)
	{
		assert(section.count >= 1u);
		assert(section.type == ObjectType::LINE);

		const auto maxGeometryBufferPoints = (maxGeometryBufferSize - currentGeometryBufferSize) / sizeof(float64_t2);
		const auto maxGeometryBufferLines = (maxGeometryBufferPoints <= 1u) ? 0u : maxGeometryBufferPoints - 1u;

		uint32_t uploadableObjects = (maxIndices - currentIndexCount) / 6u;
		uploadableObjects = core::min(uploadableObjects, maxGeometryBufferLines);
		uploadableObjects = core::min(uploadableObjects, maxDrawObjects - currentDrawObjectCount);

		const auto lineCount = section.count;
		const auto remainingObjects = lineCount - currentObjectInSection;
		uint32_t objectsToUpload = core::min(uploadableObjects, remainingObjects);

		// Add Indices
		addPolylineObjectIndices_Internal(currentDrawObjectCount, objectsToUpload);

		// Add DrawObjs
		DrawObject drawObj = {};
		drawObj.mainObjIndex = mainObjIdx;
		drawObj.type_subsectionIdx = uint32_t(static_cast<uint16_t>(ObjectType::LINE) | 0 << 16);
		drawObj.geometryAddress = geometryBufferAddress + currentGeometryBufferSize;
		for (uint32_t i = 0u; i < objectsToUpload; ++i)
		{
			void* dst = reinterpret_cast<DrawObject*>(cpuDrawBuffers.drawObjectsBuffer->getPointer()) + currentDrawObjectCount;
			memcpy(dst, &drawObj, sizeof(DrawObject));
			currentDrawObjectCount += 1u;
			drawObj.geometryAddress += sizeof(float64_t2);
		}

		// Add Geometry
		if (objectsToUpload > 0u)
		{
			const auto pointsByteSize = sizeof(float64_t2) * (objectsToUpload + 1u);
			void* dst = reinterpret_cast<char*>(cpuDrawBuffers.geometryBuffer->getPointer()) + currentGeometryBufferSize;
			auto& linePoint = polyline.getLinePointAt(section.index + currentObjectInSection);
			memcpy(dst, &linePoint, pointsByteSize);
			currentGeometryBufferSize += pointsByteSize;
		}

		currentObjectInSection += objectsToUpload;
	}

	void addQuadBeziers_Internal(const CPolyline& polyline, const CPolyline::SectionInfo& section, uint32_t& currentObjectInSection, uint32_t mainObjIdx)
	{
		// TODO[Przemek]: Beziers are added here, understand how this function works, may come in handy
		constexpr uint32_t CagesPerQuadBezier = getCageCountPerPolylineObject(ObjectType::QUAD_BEZIER);
		constexpr uint32_t IndicesPerQuadBezier	= 6u * CagesPerQuadBezier;
		assert(section.type == ObjectType::QUAD_BEZIER);

		const auto maxGeometryBufferEllipses = (maxGeometryBufferSize - currentGeometryBufferSize) / sizeof(QuadraticBezierInfo);

		uint32_t uploadableObjects = (maxIndices - currentIndexCount) / IndicesPerQuadBezier;
		uploadableObjects = core::min(uploadableObjects, maxGeometryBufferEllipses);
		uploadableObjects = core::min(uploadableObjects, maxDrawObjects - currentDrawObjectCount);

		const auto beziersCount = section.count;
		const auto remainingObjects = beziersCount - currentObjectInSection;
		uint32_t objectsToUpload = core::min(uploadableObjects, remainingObjects);

		// Add Indices
		addPolylineObjectIndices_Internal(currentDrawObjectCount, objectsToUpload * CagesPerQuadBezier);

		// Add DrawObjs
		DrawObject drawObj = {};
		drawObj.mainObjIndex = mainObjIdx;
		drawObj.geometryAddress = geometryBufferAddress + currentGeometryBufferSize;
		for (uint32_t i = 0u; i < objectsToUpload; ++i)
		{
			for (uint16_t subObject = 0; subObject < CagesPerQuadBezier; subObject++)
			{
				drawObj.type_subsectionIdx = uint32_t(static_cast<uint16_t>(ObjectType::QUAD_BEZIER) | (subObject << 16));
				void* dst = reinterpret_cast<DrawObject*>(cpuDrawBuffers.drawObjectsBuffer->getPointer()) + currentDrawObjectCount;
				memcpy(dst, &drawObj, sizeof(DrawObject));
				currentDrawObjectCount += 1u;
			}
			drawObj.geometryAddress += sizeof(QuadraticBezierInfo);
		}

		// Add Geometry
		if (objectsToUpload > 0u)
		{
			const auto beziersByteSize = sizeof(QuadraticBezierInfo) * (objectsToUpload);
			void* dst = reinterpret_cast<char*>(cpuDrawBuffers.geometryBuffer->getPointer()) + currentGeometryBufferSize;
			auto& quadBezier = polyline.getQuadBezierInfoAt(section.index + currentObjectInSection);
			memcpy(dst, &quadBezier, beziersByteSize);
			currentGeometryBufferSize += beziersByteSize;
		}

		currentObjectInSection += objectsToUpload;
	}

	// TODO[Lucas] addHatch_Internal with similar signature to functions above. 
	/*
	* this does:
		- iterates through the "Boxes" in a hatch
		- finds the curves referenced by it and copies both the boxes and the curves into correct places of the geometry buffer
		- constructs drawObjs and copies them into correct place of the drawobj buffer
		- and correctly advances the memory tracker and counters
		- it will iterate to a point where it ends OR there is not enough memory left
		- For example we might have enough memory left for 10 curve boxes in the geometry buffer,
			but that doesn't mean we could draw 10 curve boxes because their curves need to also reside in mem,
			the solution is simple when we iterate on curve boxes and keep track of what curves we have already copied into mem (a map from cpuCurveIndex to geomBufferOffset)
	*/

	//@param oddProvokingVertex is used for our polyline-wide transparency algorithm where we draw the object twice, once to resolve the alpha and another time to draw them
	void addPolylineObjectIndices_Internal(uint32_t startObject, uint32_t objectCount)
	{
		constexpr bool oddProvokingVertex = true; // was useful before, might probably deprecate it later for simplicity or it might be useful for some tricks later on
		index_buffer_type* indices = reinterpret_cast<index_buffer_type*>(cpuDrawBuffers.indexBuffer->getPointer()) + currentIndexCount;
		for (uint32_t i = 0u; i < objectCount; ++i)
		{
			index_buffer_type objIndex = startObject + i;
			if (oddProvokingVertex)
			{
				indices[i * 6] = objIndex * 4u + 1u;
				indices[i * 6 + 1u] = objIndex * 4u + 0u;
			}
			else
			{
				indices[i * 6] = objIndex * 4u + 0u;
				indices[i * 6 + 1u] = objIndex * 4u + 1u;
			}
			indices[i * 6 + 2u] = objIndex * 4u + 2u;

			if (oddProvokingVertex)
			{
				indices[i * 6 + 3u] = objIndex * 4u + 1u;
				indices[i * 6 + 4u] = objIndex * 4u + 2u;
			}
			else
			{
				indices[i * 6 + 3u] = objIndex * 4u + 2u;
				indices[i * 6 + 4u] = objIndex * 4u + 1u;
			}
			indices[i * 6 + 5u] = objIndex * 4u + 3u;
		}
		currentIndexCount += objectCount * 6u;
	}

	void resetAllCounters()
	{
		resetMainObjectCounters();
		resetGeometryCounters();
		resetIndexCounters();
		resetStyleCounters();
		resetCustomClipProjectionCounters();
	}

	void resetMainObjectCounters()
	{
		inMemMainObjectCount = 0u;
		currentMainObjectCount = 0u;
	}

	void resetGeometryCounters()
	{
		inMemDrawObjectCount = 0u;
		currentDrawObjectCount = 0u;

		inMemGeometryBufferSize = 0u;
		currentGeometryBufferSize = 0u;
	}

	void resetIndexCounters()
	{
		inMemIndexCount = 0u;
		currentIndexCount = 0u;
	}

	void resetStyleCounters()
	{
		currentLineStylesCount = 0u;
		inMemLineStylesCount = 0u;
	}
	
	void resetCustomClipProjectionCounters()
	{
		currentClipProjectionDataCount = 0u;
		inMemClipProjectionDataCount = 0u;
	}

	core::smart_refctd_ptr<nbl::video::IUtilities> utilities;
	core::smart_refctd_ptr<nbl::video::ILogicalDevice> device;

	uint32_t inMemIndexCount = 0u;
	uint32_t currentIndexCount = 0u;
	uint32_t maxIndices = 0u;

	uint32_t inMemMainObjectCount = 0u;
	uint32_t currentMainObjectCount = 0u;
	uint32_t maxMainObjects = 0u;

	uint32_t inMemDrawObjectCount = 0u;
	uint32_t currentDrawObjectCount = 0u;
	uint32_t maxDrawObjects = 0u;

	uint64_t inMemGeometryBufferSize = 0u;
	uint64_t currentGeometryBufferSize = 0u;
	uint64_t maxGeometryBufferSize = 0u;

	uint32_t inMemLineStylesCount = 0u;
	uint32_t currentLineStylesCount = 0u;
	uint32_t maxLineStyles = 0u;

	uint32_t inMemClipProjectionDataCount = 0u;
	uint32_t currentClipProjectionDataCount = 0u;
	uint32_t maxClipProjectionData = 0u;

	uint64_t geometryBufferAddress = 0u; // Actual BDA offset 0 of the gpu buffer
};

class CADApp : public ApplicationBase
{
	constexpr static uint32_t FRAMES_IN_FLIGHT = 3u;
	static constexpr uint64_t MAX_TIMEOUT = 99999999999999ull;

	constexpr static uint32_t WIN_W = 1600u;
	constexpr static uint32_t WIN_H = 900u;

	CommonAPI::InputSystem::ChannelReader<IMouseEventChannel> mouse;
	CommonAPI::InputSystem::ChannelReader<IKeyboardEventChannel> keyboard;

	core::smart_refctd_ptr<video::IQueryPool> pipelineStatsPool;

	core::smart_refctd_ptr<nbl::ui::IWindowManager> windowManager;
	core::smart_refctd_ptr<nbl::ui::IWindow> window;
	core::smart_refctd_ptr<CommonAPI::CommonAPIEventCallback> windowCb;
	core::smart_refctd_ptr<nbl::video::IAPIConnection> apiConnection;
	core::smart_refctd_ptr<nbl::video::ISurface> surface;
	core::smart_refctd_ptr<nbl::video::IUtilities> utilities;
	core::smart_refctd_ptr<nbl::video::ILogicalDevice> logicalDevice;
	video::IPhysicalDevice* physicalDevice;
	std::array<video::IGPUQueue*, CommonAPI::InitOutput::MaxQueuesCount> queues;
	core::smart_refctd_ptr<nbl::video::ISwapchain> swapchain;
	core::smart_refctd_ptr<nbl::video::IGPURenderpass> renderpassInitial; // this renderpass will clear the attachment and transition it to COLOR_ATTACHMENT_OPTIMAL
	core::smart_refctd_ptr<nbl::video::IGPURenderpass> renderpassInBetween; // this renderpass will load the attachment and transition it to COLOR_ATTACHMENT_OPTIMAL
	core::smart_refctd_ptr<nbl::video::IGPURenderpass> renderpassFinal; // this renderpass will load the attachment and transition it to PRESENT
	nbl::core::smart_refctd_dynamic_array<nbl::core::smart_refctd_ptr<nbl::video::IGPUFramebuffer>> framebuffersDynArraySmartPtr;
	std::array<std::array<nbl::core::smart_refctd_ptr<nbl::video::IGPUCommandPool>, CommonAPI::InitOutput::MaxFramesInFlight>, CommonAPI::InitOutput::MaxQueuesCount> commandPools;
	core::smart_refctd_ptr<nbl::system::ISystem> system;
	core::smart_refctd_ptr<nbl::asset::IAssetManager> assetManager;
	video::IGPUObjectFromAssetConverter::SParams cpu2gpuParams;
	core::smart_refctd_ptr<nbl::system::ILogger> logger;
	core::smart_refctd_ptr<CommonAPI::InputSystem> inputSystem;
	video::IGPUObjectFromAssetConverter cpu2gpu;
	core::smart_refctd_ptr<video::IGPUImage> m_swapchainImages[CommonAPI::InitOutput::MaxSwapChainImageCount];

	int32_t m_resourceIx = -1;
	uint32_t m_SwapchainImageIx = ~0u;

	core::smart_refctd_ptr<video::IGPUSemaphore> m_imageAcquire[FRAMES_IN_FLIGHT] = { nullptr };
	core::smart_refctd_ptr<video::IGPUSemaphore> m_renderFinished[FRAMES_IN_FLIGHT] = { nullptr };
	core::smart_refctd_ptr<video::IGPUFence> m_frameComplete[FRAMES_IN_FLIGHT] = { nullptr };

	core::smart_refctd_ptr<video::IGPUCommandBuffer> m_cmdbuf[FRAMES_IN_FLIGHT] = { nullptr };
	core::smart_refctd_ptr<video::IGPUCommandBuffer> m_uploadCmdBuf[FRAMES_IN_FLIGHT] = { nullptr };

	nbl::video::ISwapchain::SCreationParams m_swapchainCreationParams;

	// Related to Drawing Stuff
	Camera2D m_Camera;

	core::smart_refctd_ptr<video::IGPUImageView> pseudoStencilImageView[FRAMES_IN_FLIGHT];
	core::smart_refctd_ptr<video::IGPUBuffer> globalsBuffer[FRAMES_IN_FLIGHT];
	core::smart_refctd_ptr<video::IGPUDescriptorSet> descriptorSets[FRAMES_IN_FLIGHT];
	core::smart_refctd_ptr<video::IGPUGraphicsPipeline> graphicsPipeline;
	core::smart_refctd_ptr<video::IGPUGraphicsPipeline> debugGraphicsPipeline;
	core::smart_refctd_ptr<video::IGPUDescriptorSetLayout> descriptorSetLayout;
	core::smart_refctd_ptr<video::IGPUPipelineLayout> graphicsPipelineLayout;

	core::smart_refctd_ptr<video::IGPUGraphicsPipeline> resolveAlphaGraphicsPipeline;
	core::smart_refctd_ptr<video::IGPUPipelineLayout> resolveAlphaPipeLayout;

	DrawBuffersFiller drawBuffers[FRAMES_IN_FLIGHT];
	CPolyline bigPolyline;
	CPolyline bigPolyline2;

	bool fragmentShaderInterlockEnabled = false;

	// TODO: Needs better info about regular scenes and main limiters to improve the allocations in this function
	void initDrawObjects(uint32_t maxObjects)
	{
		for (uint32_t i = 0u; i < FRAMES_IN_FLIGHT; ++i)
		{
			drawBuffers[i] = DrawBuffersFiller(core::smart_refctd_ptr(utilities));

			size_t maxIndices = maxObjects * 6u * 2u;
			drawBuffers[i].allocateIndexBuffer(logicalDevice, maxIndices);
			drawBuffers[i].allocateMainObjectsBuffer(logicalDevice, maxObjects);
			drawBuffers[i].allocateDrawObjectsBuffer(logicalDevice, maxObjects * 5u);
			drawBuffers[i].allocateStylesBuffer(logicalDevice, 16u);
			drawBuffers[i].allocateCustomClipProjectionBuffer(logicalDevice, 128u);

			// * 3 because I just assume there is on average 3x beziers per actual object (cause we approximate other curves/arcs with beziers now)
			size_t geometryBufferSize = maxObjects * sizeof(QuadraticBezierInfo) * 3;
			drawBuffers[i].allocateGeometryBuffer(logicalDevice, geometryBufferSize);
		}

		for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i)
		{
			video::IGPUBuffer::SCreationParams globalsCreationParams = {};
			globalsCreationParams.size = sizeof(Globals);
			globalsCreationParams.usage = video::IGPUBuffer::EUF_UNIFORM_BUFFER_BIT | video::IGPUBuffer::EUF_TRANSFER_DST_BIT | video::IGPUBuffer::EUF_INLINE_UPDATE_VIA_CMDBUF;
			globalsBuffer[i] = logicalDevice->createBuffer(std::move(globalsCreationParams));

			video::IDeviceMemoryBacked::SDeviceMemoryRequirements memReq = globalsBuffer[i]->getMemoryReqs();
			memReq.memoryTypeBits &= physicalDevice->getDeviceLocalMemoryTypeBits();
			auto globalsBufferMem = logicalDevice->allocate(memReq, globalsBuffer[i].get());
		}

		// pseudoStencil

		asset::E_FORMAT pseudoStencilFormat = asset::EF_R32_UINT;

		video::IPhysicalDevice::SImageFormatPromotionRequest promotionRequest = {};
		promotionRequest.originalFormat = asset::EF_R32_UINT;
		promotionRequest.usages = {};
		promotionRequest.usages.storageImageAtomic = true;
		pseudoStencilFormat = physicalDevice->promoteImageFormat(promotionRequest, video::IGPUImage::ET_OPTIMAL);

		for (uint32_t i = 0u; i < FRAMES_IN_FLIGHT; ++i)
		{
			video::IGPUImage::SCreationParams imgInfo;
			imgInfo.format = pseudoStencilFormat;
			imgInfo.type = video::IGPUImage::ET_2D;
			imgInfo.extent.width = WIN_W;
			imgInfo.extent.height = WIN_H;
			imgInfo.extent.depth = 1u;
			imgInfo.mipLevels = 1u;
			imgInfo.arrayLayers = 1u;
			imgInfo.samples = asset::ICPUImage::ESCF_1_BIT;
			imgInfo.flags = asset::IImage::E_CREATE_FLAGS::ECF_NONE;
			imgInfo.usage = asset::IImage::EUF_STORAGE_BIT | asset::IImage::EUF_TRANSFER_DST_BIT;
			imgInfo.initialLayout = video::IGPUImage::EL_UNDEFINED;
			imgInfo.tiling = video::IGPUImage::ET_OPTIMAL;

			auto image = logicalDevice->createImage(std::move(imgInfo));
			auto imageMemReqs = image->getMemoryReqs();
			imageMemReqs.memoryTypeBits &= logicalDevice->getPhysicalDevice()->getDeviceLocalMemoryTypeBits();
			logicalDevice->allocate(imageMemReqs, image.get());

			image->setObjectDebugName("pseudoStencil Image");

			video::IGPUImageView::SCreationParams imgViewInfo;
			imgViewInfo.image = std::move(image);
			imgViewInfo.format = pseudoStencilFormat;
			imgViewInfo.viewType = video::IGPUImageView::ET_2D;
			imgViewInfo.flags = video::IGPUImageView::E_CREATE_FLAGS::ECF_NONE;
			imgViewInfo.subresourceRange.aspectMask = asset::IImage::E_ASPECT_FLAGS::EAF_COLOR_BIT;
			imgViewInfo.subresourceRange.baseArrayLayer = 0u;
			imgViewInfo.subresourceRange.baseMipLevel = 0u;
			imgViewInfo.subresourceRange.layerCount = 1u;
			imgViewInfo.subresourceRange.levelCount = 1u;

			pseudoStencilImageView[i] = logicalDevice->createImageView(std::move(imgViewInfo));
		}
	}

public:
	void setWindow(core::smart_refctd_ptr<nbl::ui::IWindow>&& wnd) override
	{
		window = std::move(wnd);
	}
	nbl::ui::IWindow* getWindow() override
	{
		return window.get();
	}
	void setSystem(core::smart_refctd_ptr<nbl::system::ISystem>&& system) override
	{
		system = std::move(system);
	}
	video::IAPIConnection* getAPIConnection() override
	{
		return apiConnection.get();
	}
	video::ILogicalDevice* getLogicalDevice()  override
	{
		return logicalDevice.get();
	}
	video::IGPURenderpass* getRenderpass() override
	{
		return renderpassFinal.get();
	}
	void setSurface(core::smart_refctd_ptr<video::ISurface>&& s) override
	{
		surface = std::move(s);
	}
	void setFBOs(std::vector<core::smart_refctd_ptr<video::IGPUFramebuffer>>& f) override
	{
		for (int i = 0; i < f.size(); i++)
		{
			auto& fboDynArray = *(framebuffersDynArraySmartPtr.get());
			fboDynArray[i] = core::smart_refctd_ptr(f[i]);
		}
	}
	void setSwapchain(core::smart_refctd_ptr<video::ISwapchain>&& s) override
	{
		swapchain = std::move(s);
	}
	uint32_t getSwapchainImageCount() override
	{
		return swapchain->getImageCount();
	}
	virtual nbl::asset::E_FORMAT getDepthFormat() override
	{
		return nbl::asset::EF_UNKNOWN;
	}

	nbl::core::smart_refctd_ptr<nbl::video::IGPURenderpass> createRenderpass(
		nbl::asset::E_FORMAT colorAttachmentFormat,
		nbl::asset::E_FORMAT baseDepthFormat,
		nbl::video::IGPURenderpass::E_LOAD_OP loadOp,
		nbl::asset::IImage::E_LAYOUT initialLayout,
		nbl::asset::IImage::E_LAYOUT finalLayout)
	{
		using namespace nbl;

		bool useDepth = baseDepthFormat != nbl::asset::EF_UNKNOWN;
		nbl::asset::E_FORMAT depthFormat = nbl::asset::EF_UNKNOWN;
		if (useDepth)
		{
			depthFormat = logicalDevice->getPhysicalDevice()->promoteImageFormat(
				{ baseDepthFormat, nbl::video::IPhysicalDevice::SFormatImageUsages::SUsage(nbl::asset::IImage::EUF_DEPTH_STENCIL_ATTACHMENT_BIT) },
				nbl::video::IGPUImage::ET_OPTIMAL
			);
			assert(depthFormat != nbl::asset::EF_UNKNOWN);
		}

		nbl::video::IGPURenderpass::SCreationParams::SAttachmentDescription attachments[2];
		attachments[0].initialLayout = initialLayout;
		attachments[0].finalLayout = finalLayout;
		attachments[0].format = colorAttachmentFormat;
		attachments[0].samples = asset::IImage::ESCF_1_BIT;
		attachments[0].loadOp = loadOp;
		attachments[0].storeOp = nbl::video::IGPURenderpass::ESO_STORE;

		attachments[1].initialLayout = asset::IImage::EL_UNDEFINED;
		attachments[1].finalLayout = asset::IImage::EL_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		attachments[1].format = depthFormat;
		attachments[1].samples = asset::IImage::ESCF_1_BIT;
		attachments[1].loadOp = loadOp;
		attachments[1].storeOp = nbl::video::IGPURenderpass::ESO_STORE;

		nbl::video::IGPURenderpass::SCreationParams::SSubpassDescription::SAttachmentRef colorAttRef;
		colorAttRef.attachment = 0u;
		colorAttRef.layout = asset::IImage::EL_COLOR_ATTACHMENT_OPTIMAL;

		nbl::video::IGPURenderpass::SCreationParams::SSubpassDescription::SAttachmentRef depthStencilAttRef;
		depthStencilAttRef.attachment = 1u;
		depthStencilAttRef.layout = asset::IImage::EL_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		nbl::video::IGPURenderpass::SCreationParams::SSubpassDescription sp;
		sp.pipelineBindPoint = asset::EPBP_GRAPHICS;
		sp.colorAttachmentCount = 1u;
		sp.colorAttachments = &colorAttRef;
		if (useDepth) {
			sp.depthStencilAttachment = &depthStencilAttRef;
		}
		else {
			sp.depthStencilAttachment = nullptr;
		}
		sp.flags = nbl::video::IGPURenderpass::ESDF_NONE;
		sp.inputAttachmentCount = 0u;
		sp.inputAttachments = nullptr;
		sp.preserveAttachmentCount = 0u;
		sp.preserveAttachments = nullptr;
		sp.resolveAttachments = nullptr;

		nbl::video::IGPURenderpass::SCreationParams rp_params;
		rp_params.attachmentCount = (useDepth) ? 2u : 1u;
		rp_params.attachments = attachments;
		rp_params.dependencies = nullptr;
		rp_params.dependencyCount = 0u;
		rp_params.subpasses = &sp;
		rp_params.subpassCount = 1u;

		return logicalDevice->createRenderpass(rp_params);
	}

	void getAndLogQueryPoolResults()
	{
#ifdef BEZIER_CAGE_ADAPTIVE_T_FIND // results for bezier show an optimal number of 0.14 for T
		{
			uint32_t samples_passed[1] = {};
			auto queryResultFlags = core::bitflag<video::IQueryPool::E_QUERY_RESULTS_FLAGS>(video::IQueryPool::EQRF_WAIT_BIT);
			logicalDevice->getQueryPoolResults(pipelineStatsPool.get(), 0u, 1u, sizeof(samples_passed), samples_passed, sizeof(uint32_t), queryResultFlags);
			logger->log("[WAIT] SamplesPassed[0] = %d", system::ILogger::ELL_INFO, samples_passed[0]);
			std::cout << MinT << ", " << PrevSamples << std::endl;
			if (PrevSamples > samples_passed[0]) {
				PrevSamples = samples_passed[0];
				MinT = (sin(T) + 1.01f) / 4.03f;
			}
		}
#endif
	}

	APP_CONSTRUCTOR(CADApp);

	void onAppInitialized_impl() override
	{
		const auto swapchainImageUsage = static_cast<asset::IImage::E_USAGE_FLAGS>(asset::IImage::EUF_COLOR_ATTACHMENT_BIT);
		std::array<asset::E_FORMAT, 1> acceptableSurfaceFormats = { asset::EF_B8G8R8A8_UNORM };

		CommonAPI::InitParams initParams;
		initParams.windowCb = core::smart_refctd_ptr<CommonAPI::CommonAPIEventCallback>(this);
		initParams.window = core::smart_refctd_ptr(window);
		initParams.apiType = video::EAT_VULKAN;
		initParams.appName = { _NBL_APP_NAME_ };
		initParams.framesInFlight = FRAMES_IN_FLIGHT;
		initParams.windowWidth = WIN_W;
		initParams.windowHeight = WIN_H;
		initParams.swapchainImageCount = 3u;
		initParams.swapchainImageUsage = swapchainImageUsage;
		initParams.depthFormat = getDepthFormat();
		initParams.acceptableSurfaceFormats = acceptableSurfaceFormats.data();
		initParams.acceptableSurfaceFormatCount = acceptableSurfaceFormats.size();
		initParams.physicalDeviceFilter.requiredFeatures.bufferDeviceAddress = true;
		initParams.physicalDeviceFilter.requiredFeatures.shaderFloat64 = true;
		initParams.physicalDeviceFilter.requiredFeatures.fillModeNonSolid = DebugMode;
		initParams.physicalDeviceFilter.requiredFeatures.fragmentShaderPixelInterlock = FragmentShaderPixelInterlock;
		initParams.physicalDeviceFilter.requiredFeatures.pipelineStatisticsQuery = true;
		initParams.physicalDeviceFilter.requiredFeatures.shaderClipDistance = true;
		initParams.physicalDeviceFilter.requiredFeatures.scalarBlockLayout = true;
		auto initOutput = CommonAPI::InitWithDefaultExt(std::move(initParams));

		system = std::move(initOutput.system);
		window = std::move(initParams.window);
		windowCb = std::move(initParams.windowCb);
		apiConnection = std::move(initOutput.apiConnection);
		surface = std::move(initOutput.surface);
		physicalDevice = std::move(initOutput.physicalDevice);
		logicalDevice = std::move(initOutput.logicalDevice);
		utilities = std::move(initOutput.utilities);
		queues = std::move(initOutput.queues);
		assetManager = std::move(initOutput.assetManager);
		cpu2gpuParams = std::move(initOutput.cpu2gpuParams);
		logger = std::move(initOutput.logger);
		inputSystem = std::move(initOutput.inputSystem);
		windowManager = std::move(initOutput.windowManager);
		// renderpass = std::move(initOutput.renderToSwapchainRenderpass);
		m_swapchainCreationParams = std::move(initOutput.swapchainCreationParams);

		fragmentShaderInterlockEnabled = logicalDevice->getEnabledFeatures().fragmentShaderPixelInterlock;

		{
			video::IQueryPool::SCreationParams queryPoolCreationParams = {};
			queryPoolCreationParams.queryType = video::IQueryPool::EQT_PIPELINE_STATISTICS;
			queryPoolCreationParams.queryCount = 1u;
			queryPoolCreationParams.pipelineStatisticsFlags = video::IQueryPool::EPSF_FRAGMENT_SHADER_INVOCATIONS_BIT;
			pipelineStatsPool = logicalDevice->createQueryPool(std::move(queryPoolCreationParams));
		}


		renderpassInitial = createRenderpass(m_swapchainCreationParams.surfaceFormat.format, getDepthFormat(), nbl::video::IGPURenderpass::ELO_CLEAR, asset::IImage::EL_UNDEFINED, asset::IImage::EL_COLOR_ATTACHMENT_OPTIMAL);
		renderpassInBetween = createRenderpass(m_swapchainCreationParams.surfaceFormat.format, getDepthFormat(), nbl::video::IGPURenderpass::ELO_LOAD, asset::IImage::EL_COLOR_ATTACHMENT_OPTIMAL, asset::IImage::EL_COLOR_ATTACHMENT_OPTIMAL);
		renderpassFinal = createRenderpass(m_swapchainCreationParams.surfaceFormat.format, getDepthFormat(), nbl::video::IGPURenderpass::ELO_LOAD, asset::IImage::EL_COLOR_ATTACHMENT_OPTIMAL, asset::IImage::EL_PRESENT_SRC);

		commandPools = std::move(initOutput.commandPools);
		const auto& graphicsCommandPools = commandPools[CommonAPI::InitOutput::EQT_GRAPHICS];
		const auto& transferCommandPools = commandPools[CommonAPI::InitOutput::EQT_TRANSFER_UP];

		CommonAPI::createSwapchain(std::move(logicalDevice), m_swapchainCreationParams, WIN_W, WIN_H, swapchain);

		framebuffersDynArraySmartPtr = CommonAPI::createFBOWithSwapchainImages(
			swapchain->getImageCount(), WIN_W, WIN_H,
			logicalDevice, swapchain, renderpassFinal,
			getDepthFormat()
		);

		const uint32_t swapchainImageCount = swapchain->getImageCount();
		for (uint32_t i = 0; i < swapchainImageCount; ++i)
		{
			auto& fboDynArray = *(framebuffersDynArraySmartPtr.get());
			m_swapchainImages[i] = fboDynArray[i]->getCreationParameters().attachments[0u]->getCreationParameters().image;
		}

		video::IGPUObjectFromAssetConverter CPU2GPU;

		core::smart_refctd_ptr<video::IGPUSpecializedShader> shaders[4u] = {};
		{
			asset::IAssetLoader::SAssetLoadParams params = {};
			params.logger = logger.get();
			core::smart_refctd_ptr<asset::ICPUSpecializedShader> cpuShaders[4u] = {};
			constexpr auto vertexShaderPath = "../vertex_shader.hlsl";
			constexpr auto fragmentShaderPath = "../fragment_shader.hlsl";
			constexpr auto debugfragmentShaderPath = "../fragment_shader_debug.hlsl";
			constexpr auto resolveAlphasShaderPath = "../resolve_alphas.hlsl";
			cpuShaders[0u] = core::smart_refctd_ptr_static_cast<asset::ICPUSpecializedShader>(*assetManager->getAsset(vertexShaderPath, params).getContents().begin());
			cpuShaders[1u] = core::smart_refctd_ptr_static_cast<asset::ICPUSpecializedShader>(*assetManager->getAsset(fragmentShaderPath, params).getContents().begin());
			cpuShaders[2u] = core::smart_refctd_ptr_static_cast<asset::ICPUSpecializedShader>(*assetManager->getAsset(debugfragmentShaderPath, params).getContents().begin());
			cpuShaders[3u] = core::smart_refctd_ptr_static_cast<asset::ICPUSpecializedShader>(*assetManager->getAsset(resolveAlphasShaderPath, params).getContents().begin());
			cpuShaders[0u]->setSpecializationInfo(asset::ISpecializedShader::SInfo(nullptr, nullptr, "main"));
			cpuShaders[1u]->setSpecializationInfo(asset::ISpecializedShader::SInfo(nullptr, nullptr, "main"));
			cpuShaders[2u]->setSpecializationInfo(asset::ISpecializedShader::SInfo(nullptr, nullptr, "main"));
			cpuShaders[3u]->setSpecializationInfo(asset::ISpecializedShader::SInfo(nullptr, nullptr, "main"));
			auto gpuShaders = CPU2GPU.getGPUObjectsFromAssets(cpuShaders, cpuShaders + 4u, cpu2gpuParams);
			shaders[0u] = gpuShaders->begin()[0u];
			shaders[1u] = gpuShaders->begin()[1u];
			shaders[2u] = gpuShaders->begin()[2u];
			shaders[3u] = gpuShaders->begin()[3u];
		}

		initDrawObjects(20480u);

		// Create DescriptorSetLayout, PipelineLayout and update DescriptorSets
		{
			constexpr uint32_t BindingCount = 6u;
			video::IGPUDescriptorSetLayout::SBinding bindings[BindingCount] = {};
			bindings[0].binding = 0u;
			bindings[0].type = asset::IDescriptor::E_TYPE::ET_UNIFORM_BUFFER;
			bindings[0].count = 1u;
			bindings[0].stageFlags = asset::IShader::ESS_VERTEX | asset::IShader::ESS_FRAGMENT;

			bindings[1].binding = 1u;
			bindings[1].type = asset::IDescriptor::E_TYPE::ET_STORAGE_BUFFER;
			bindings[1].count = 1u;
			bindings[1].stageFlags = asset::IShader::ESS_VERTEX | asset::IShader::ESS_FRAGMENT;

			bindings[2].binding = 2u;
			bindings[2].type = asset::IDescriptor::E_TYPE::ET_STORAGE_IMAGE;
			bindings[2].count = 1u;
			bindings[2].stageFlags = asset::IShader::ESS_FRAGMENT;

			bindings[3].binding = 3u;
			bindings[3].type = asset::IDescriptor::E_TYPE::ET_STORAGE_BUFFER;
			bindings[3].count = 1u;
			bindings[3].stageFlags = asset::IShader::ESS_VERTEX | asset::IShader::ESS_FRAGMENT;

			bindings[4].binding = 4u;
			bindings[4].type = asset::IDescriptor::E_TYPE::ET_STORAGE_BUFFER;
			bindings[4].count = 1u;
			bindings[4].stageFlags = asset::IShader::ESS_VERTEX | asset::IShader::ESS_FRAGMENT;

			bindings[5].binding = 5u;
			bindings[5].type = asset::IDescriptor::E_TYPE::ET_STORAGE_BUFFER;
			bindings[5].count = 1u;
			bindings[5].stageFlags = asset::IShader::ESS_VERTEX;

			descriptorSetLayout = logicalDevice->createDescriptorSetLayout(bindings, bindings + BindingCount);

			nbl::core::smart_refctd_ptr<nbl::video::IDescriptorPool> descriptorPool = nullptr;
			{
				nbl::video::IDescriptorPool::SCreateInfo createInfo = {};
				createInfo.flags = nbl::video::IDescriptorPool::ECF_NONE;
				createInfo.maxSets = 128u;
				createInfo.maxDescriptorCount[static_cast<uint32_t>(nbl::asset::IDescriptor::E_TYPE::ET_UNIFORM_BUFFER)] = FRAMES_IN_FLIGHT;
				createInfo.maxDescriptorCount[static_cast<uint32_t>(nbl::asset::IDescriptor::E_TYPE::ET_STORAGE_BUFFER)] = 4 * FRAMES_IN_FLIGHT;
				createInfo.maxDescriptorCount[static_cast<uint32_t>(nbl::asset::IDescriptor::E_TYPE::ET_STORAGE_IMAGE)] = FRAMES_IN_FLIGHT;

				descriptorPool = logicalDevice->createDescriptorPool(std::move(createInfo));
			}

			for (size_t i = 0; i < FRAMES_IN_FLIGHT; i++)
			{
				descriptorSets[i] = descriptorPool->createDescriptorSet(core::smart_refctd_ptr(descriptorSetLayout));
				video::IGPUDescriptorSet::SDescriptorInfo descriptorInfos[6u] = {};
				descriptorInfos[0u].info.buffer.offset = 0u;
				descriptorInfos[0u].info.buffer.size = globalsBuffer[i]->getCreationParams().size;
				descriptorInfos[0u].desc = globalsBuffer[i];

				descriptorInfos[1u].info.buffer.offset = 0u;
				descriptorInfos[1u].info.buffer.size = drawBuffers[i].gpuDrawBuffers.drawObjectsBuffer->getCreationParams().size;
				descriptorInfos[1u].desc = drawBuffers[i].gpuDrawBuffers.drawObjectsBuffer;

				descriptorInfos[2u].info.image.imageLayout = asset::IImage::E_LAYOUT::EL_GENERAL;
				descriptorInfos[2u].info.image.sampler = nullptr;
				descriptorInfos[2u].desc = pseudoStencilImageView[i];

				descriptorInfos[3u].info.buffer.offset = 0u;
				descriptorInfos[3u].info.buffer.size = drawBuffers[i].gpuDrawBuffers.lineStylesBuffer->getCreationParams().size;
				descriptorInfos[3u].desc = drawBuffers[i].gpuDrawBuffers.lineStylesBuffer;

				descriptorInfos[4u].info.buffer.offset = 0u;
				descriptorInfos[4u].info.buffer.size = drawBuffers[i].gpuDrawBuffers.mainObjectsBuffer->getCreationParams().size;
				descriptorInfos[4u].desc = drawBuffers[i].gpuDrawBuffers.mainObjectsBuffer;

				descriptorInfos[5u].info.buffer.offset = 0u;
				descriptorInfos[5u].info.buffer.size = drawBuffers[i].gpuDrawBuffers.customClipProjectionBuffer->getCreationParams().size;
				descriptorInfos[5u].desc = drawBuffers[i].gpuDrawBuffers.customClipProjectionBuffer;

				video::IGPUDescriptorSet::SWriteDescriptorSet descriptorUpdates[6u] = {};
				descriptorUpdates[0u].dstSet = descriptorSets[i].get();
				descriptorUpdates[0u].binding = 0u;
				descriptorUpdates[0u].arrayElement = 0u;
				descriptorUpdates[0u].count = 1u;
				descriptorUpdates[0u].descriptorType = asset::IDescriptor::E_TYPE::ET_UNIFORM_BUFFER;
				descriptorUpdates[0u].info = &descriptorInfos[0u];

				descriptorUpdates[1u].dstSet = descriptorSets[i].get();
				descriptorUpdates[1u].binding = 1u;
				descriptorUpdates[1u].arrayElement = 0u;
				descriptorUpdates[1u].count = 1u;
				descriptorUpdates[1u].descriptorType = asset::IDescriptor::E_TYPE::ET_STORAGE_BUFFER;
				descriptorUpdates[1u].info = &descriptorInfos[1u];

				descriptorUpdates[2u].dstSet = descriptorSets[i].get();
				descriptorUpdates[2u].binding = 2u;
				descriptorUpdates[2u].arrayElement = 0u;
				descriptorUpdates[2u].count = 1u;
				descriptorUpdates[2u].descriptorType = asset::IDescriptor::E_TYPE::ET_STORAGE_IMAGE;
				descriptorUpdates[2u].info = &descriptorInfos[2u];

				descriptorUpdates[3u].dstSet = descriptorSets[i].get();
				descriptorUpdates[3u].binding = 3u;
				descriptorUpdates[3u].arrayElement = 0u;
				descriptorUpdates[3u].count = 1u;
				descriptorUpdates[3u].descriptorType = asset::IDescriptor::E_TYPE::ET_STORAGE_BUFFER;
				descriptorUpdates[3u].info = &descriptorInfos[3u];

				descriptorUpdates[4u].dstSet = descriptorSets[i].get();
				descriptorUpdates[4u].binding = 4u;
				descriptorUpdates[4u].arrayElement = 0u;
				descriptorUpdates[4u].count = 1u;
				descriptorUpdates[4u].descriptorType = asset::IDescriptor::E_TYPE::ET_STORAGE_BUFFER;
				descriptorUpdates[4u].info = &descriptorInfos[4u];

				descriptorUpdates[5u].dstSet = descriptorSets[i].get();
				descriptorUpdates[5u].binding = 5u;
				descriptorUpdates[5u].arrayElement = 0u;
				descriptorUpdates[5u].count = 1u;
				descriptorUpdates[5u].descriptorType = asset::IDescriptor::E_TYPE::ET_STORAGE_BUFFER;
				descriptorUpdates[5u].info = &descriptorInfos[5u];

				logicalDevice->updateDescriptorSets(6u, descriptorUpdates, 0u, nullptr);
			}

			graphicsPipelineLayout = logicalDevice->createPipelineLayout(nullptr, nullptr, core::smart_refctd_ptr(descriptorSetLayout), nullptr, nullptr, nullptr);
		}

		// Shared Blend Params between pipelines
		asset::SBlendParams blendParams = {};
		blendParams.blendParams[0u].blendEnable = true;
		blendParams.blendParams[0u].srcColorFactor = asset::EBF_SRC_ALPHA;
		blendParams.blendParams[0u].dstColorFactor = asset::EBF_ONE_MINUS_SRC_ALPHA;
		blendParams.blendParams[0u].colorBlendOp = asset::EBO_ADD;
		blendParams.blendParams[0u].srcAlphaFactor = asset::EBF_ONE;
		blendParams.blendParams[0u].dstAlphaFactor = asset::EBF_ZERO;
		blendParams.blendParams[0u].alphaBlendOp = asset::EBO_ADD;
		blendParams.blendParams[0u].colorWriteMask = (1u << 4u) - 1u;

		// Create Alpha Resovle Pipeline
		{
			auto fsTriangleProtoPipe = nbl::ext::FullScreenTriangle::createProtoPipeline(cpu2gpuParams, 0u);
			std::get<asset::SBlendParams>(fsTriangleProtoPipe) = blendParams;

			auto constants = std::get<asset::SPushConstantRange>(fsTriangleProtoPipe);
			resolveAlphaPipeLayout = logicalDevice->createPipelineLayout(&constants, &constants+1, core::smart_refctd_ptr(descriptorSetLayout), nullptr, nullptr, nullptr);
			auto fsTriangleRenderPassIndependantPipe = nbl::ext::FullScreenTriangle::createRenderpassIndependentPipeline(logicalDevice.get(), fsTriangleProtoPipe, core::smart_refctd_ptr(shaders[3u]), core::smart_refctd_ptr(resolveAlphaPipeLayout));

			video::IGPUGraphicsPipeline::SCreationParams graphicsPipelineCreateInfo = {};
			graphicsPipelineCreateInfo.renderpassIndependent = fsTriangleRenderPassIndependantPipe;
			graphicsPipelineCreateInfo.renderpass = renderpassFinal;
			resolveAlphaGraphicsPipeline = logicalDevice->createGraphicsPipeline(nullptr, std::move(graphicsPipelineCreateInfo));
		}

		// Create Main Graphics Pipelines 
		{
			video::IGPURenderpassIndependentPipeline::SCreationParams renderpassIndependantPipeInfo = {};
			renderpassIndependantPipeInfo.layout = graphicsPipelineLayout;
			renderpassIndependantPipeInfo.shaders[0u] = shaders[0u];
			renderpassIndependantPipeInfo.shaders[1u] = shaders[1u];
			// renderpassIndependantPipeInfo.vertexInput; no gpu vertex buffers
			renderpassIndependantPipeInfo.blend = blendParams;

			renderpassIndependantPipeInfo.primitiveAssembly.primitiveType = asset::E_PRIMITIVE_TOPOLOGY::EPT_TRIANGLE_LIST;
			renderpassIndependantPipeInfo.rasterization.depthTestEnable = false;
			renderpassIndependantPipeInfo.rasterization.depthWriteEnable = false;
			renderpassIndependantPipeInfo.rasterization.stencilTestEnable = false;
			renderpassIndependantPipeInfo.rasterization.polygonMode = asset::EPM_FILL;
			renderpassIndependantPipeInfo.rasterization.faceCullingMode = asset::EFCM_NONE;

			core::smart_refctd_ptr<video::IGPURenderpassIndependentPipeline> renderpassIndependant;
			bool succ = logicalDevice->createRenderpassIndependentPipelines(
				nullptr,
				core::SRange<const video::IGPURenderpassIndependentPipeline::SCreationParams>(&renderpassIndependantPipeInfo, &renderpassIndependantPipeInfo + 1u),
				&renderpassIndependant);
			assert(succ);

			video::IGPUGraphicsPipeline::SCreationParams graphicsPipelineCreateInfo = {};
			graphicsPipelineCreateInfo.renderpassIndependent = renderpassIndependant;
			graphicsPipelineCreateInfo.renderpass = renderpassFinal;
			graphicsPipeline = logicalDevice->createGraphicsPipeline(nullptr, std::move(graphicsPipelineCreateInfo));

			if constexpr (DebugMode)
			{
				core::smart_refctd_ptr<video::IGPURenderpassIndependentPipeline> renderpassIndependantDebug;
				renderpassIndependantPipeInfo.shaders[1u] = shaders[2u];
				renderpassIndependantPipeInfo.rasterization.polygonMode = asset::EPM_LINE;
				succ = logicalDevice->createRenderpassIndependentPipelines(
					nullptr,
					core::SRange<const video::IGPURenderpassIndependentPipeline::SCreationParams>(&renderpassIndependantPipeInfo, &renderpassIndependantPipeInfo + 1u),
					&renderpassIndependantDebug);
				assert(succ);

				video::IGPUGraphicsPipeline::SCreationParams debugGraphicsPipelineCreateInfo = {};
				debugGraphicsPipelineCreateInfo.renderpassIndependent = renderpassIndependantDebug;
				debugGraphicsPipelineCreateInfo.renderpass = renderpassFinal;
				debugGraphicsPipeline = logicalDevice->createGraphicsPipeline(nullptr, std::move(debugGraphicsPipelineCreateInfo));
			}
		}

		for (size_t i = 0; i < FRAMES_IN_FLIGHT; i++)
		{
			logicalDevice->createCommandBuffers(
				graphicsCommandPools[i].get(),
				video::IGPUCommandBuffer::EL_PRIMARY,
				1,
				m_cmdbuf + i);

			logicalDevice->createCommandBuffers(
				transferCommandPools[i].get(),
				video::IGPUCommandBuffer::EL_PRIMARY,
				1,
				m_uploadCmdBuf + i);
		}

		for (uint32_t i = 0u; i < FRAMES_IN_FLIGHT; i++)
		{
			m_frameComplete[i] = logicalDevice->createFence(video::IGPUFence::ECF_SIGNALED_BIT);
			m_imageAcquire[i] = logicalDevice->createSemaphore();
			m_renderFinished[i] = logicalDevice->createSemaphore();
		}

		m_Camera.setOrigin({ 0.0, 0.0 });
		m_Camera.setAspectRatio((double)WIN_W / WIN_H);
		m_Camera.setSize(200.0);

		m_timeElapsed = 0.0;


		if constexpr (mode == ExampleMode::CASE_1)
		{
			{
				std::vector<float64_t2> linePoints;
				for (uint32_t i = 0u; i < 20u; ++i)
				{
					for (uint32_t i = 0u; i < 256u; ++i)
					{
						double y = -112.0 + i * 1.1;
						linePoints.push_back({ -200.0, y });
						linePoints.push_back({ +200.0, y });
					}
					for (uint32_t i = 0u; i < 256u; ++i)
					{
						double x = -200.0 + i * 1.5;
						linePoints.push_back({ x, -100.0 });
						linePoints.push_back({ x, +100.0 });
					}
				}
				bigPolyline.addLinePoints(core::SRange<float64_t2>(linePoints.data(), linePoints.data() + linePoints.size()));
			}
			{
				std::vector<float64_t2> linePoints;
				for (uint32_t i = 0u; i < 20u; ++i)
				{
					for (uint32_t i = 0u; i < 256u; ++i)
					{
						double y = -112.0 + i * 1.1;
						double x = -200.0 + i * 1.5;
						linePoints.push_back({ -200.0 + x, y });
						linePoints.push_back({ +200.0 + x, y });
					}
					for (uint32_t i = 0u; i < 256u; ++i)
					{
						double y = -112.0 + i * 1.1;
						double x = -200.0 + i * 1.5;
						linePoints.push_back({ x, -100.0 + y });
						linePoints.push_back({ x, +100.0 + y });
					}
				}
				bigPolyline2.addLinePoints(core::SRange<float64_t2>(linePoints.data(), linePoints.data() + linePoints.size()));
			}
		}

	}

	void onAppTerminated_impl() override
	{
		logicalDevice->waitIdle();
	}

	double getScreenToWorldRatio(const float64_t3x3& viewProjectionMatrix, uint32_t2 windowSize)
	{
		double idx_0_0 = viewProjectionMatrix[0u][0u] * (windowSize.x / 2.0);
		double idx_1_1 = viewProjectionMatrix[1u][1u] * (windowSize.y / 2.0);
		double det_2x2_mat = idx_0_0 * idx_1_1;
		return core::sqrt(core::abs(det_2x2_mat));
	}

	void beginFrameRender()
	{
		auto& cb = m_cmdbuf[m_resourceIx];
		auto& commandPool = commandPools[CommonAPI::InitOutput::EQT_GRAPHICS][m_resourceIx];
		auto& fence = m_frameComplete[m_resourceIx];
		logicalDevice->blockForFences(1u, &fence.get());
		logicalDevice->resetFences(1u, &fence.get());

		m_SwapchainImageIx = 0u;
		auto acquireResult = swapchain->acquireNextImage(m_imageAcquire[m_resourceIx].get(), nullptr, &m_SwapchainImageIx);
		assert(acquireResult == video::ISwapchain::E_ACQUIRE_IMAGE_RESULT::EAIR_SUCCESS);

		core::smart_refctd_ptr<video::IGPUImage> swapchainImg = m_swapchainImages[m_SwapchainImageIx];

		// safe to proceed
		cb->reset(video::IGPUCommandBuffer::ERF_RELEASE_RESOURCES_BIT); // TODO: Begin doesn't release the resources in the command pool, meaning the old swapchains never get dropped
		cb->begin(video::IGPUCommandBuffer::EU_ONE_TIME_SUBMIT_BIT); // TODO: Reset Frame's CommandPool
		cb->beginDebugMarker("Frame");
		Globals globalData = {};
		globalData.antiAliasingFactor = 1.0f;// + abs(cos(m_timeElapsed * 0.0008))*20.0f;
		globalData.resolution = uint32_t2{ WIN_W, WIN_H };
		globalData.defaultClipProjection.projectionToNDC = m_Camera.constructViewProjection();
		globalData.defaultClipProjection.minClipNDC = float32_t2(-1.0, -1.0);
		globalData.defaultClipProjection.maxClipNDC = float32_t2(+1.0, +1.0);
		globalData.screenToWorldRatio = getScreenToWorldRatio(globalData.defaultClipProjection.projectionToNDC, globalData.resolution);
		bool updateSuccess = cb->updateBuffer(globalsBuffer[m_resourceIx].get(), 0ull, sizeof(Globals), &globalData);
		assert(updateSuccess);

		// Clear pseudoStencil
		{
			auto pseudoStencilImage = pseudoStencilImageView[m_resourceIx]->getCreationParameters().image;

			nbl::video::IGPUCommandBuffer::SImageMemoryBarrier imageBarriers[1u] = {};
			imageBarriers[0].barrier.srcAccessMask = nbl::asset::EAF_NONE;
			imageBarriers[0].barrier.dstAccessMask = nbl::asset::EAF_MEMORY_WRITE_BIT;
			imageBarriers[0].oldLayout = nbl::asset::IImage::EL_UNDEFINED;
			imageBarriers[0].newLayout = nbl::asset::IImage::EL_GENERAL;
			imageBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imageBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imageBarriers[0].image = pseudoStencilImage;
			imageBarriers[0].subresourceRange.aspectMask = nbl::asset::IImage::EAF_COLOR_BIT;
			imageBarriers[0].subresourceRange.baseMipLevel = 0u;
			imageBarriers[0].subresourceRange.levelCount = 1;
			imageBarriers[0].subresourceRange.baseArrayLayer = 0u;
			imageBarriers[0].subresourceRange.layerCount = 1;
			cb->pipelineBarrier(nbl::asset::EPSF_TOP_OF_PIPE_BIT, nbl::asset::EPSF_TRANSFER_BIT, nbl::asset::EDF_NONE, 0u, nullptr, 0u, nullptr, 1u, imageBarriers);

			uint32_t pseudoStencilInvalidValue = core::bitfieldInsert<uint32_t>(0u, InvalidMainObjectIdx, AlphaBits, MainObjectIdxBits);
			asset::SClearColorValue clear = {};
			clear.uint32[0] = pseudoStencilInvalidValue;

			asset::IImage::SSubresourceRange subresourceRange = {};
			subresourceRange.aspectMask = asset::IImage::E_ASPECT_FLAGS::EAF_COLOR_BIT;
			subresourceRange.baseArrayLayer = 0u;
			subresourceRange.baseMipLevel = 0u;
			subresourceRange.layerCount = 1u;
			subresourceRange.levelCount = 1u;

			cb->clearColorImage(pseudoStencilImage.get(), asset::IImage::EL_GENERAL, &clear, 1u, &subresourceRange);
		}

		nbl::video::IGPUCommandBuffer::SRenderpassBeginInfo beginInfo;
		{
			VkRect2D area;
			area.offset = { 0,0 };
			area.extent = { WIN_W, WIN_H };
			asset::SClearValue clear[2] = {};
			clear[0].color.float32[0] = 0.8f;
			clear[0].color.float32[1] = 0.8f;
			clear[0].color.float32[2] = 0.8f;
			clear[0].color.float32[3] = 0.f;
			clear[1].depthStencil.depth = 1.f;

			beginInfo.clearValueCount = 2u;
			beginInfo.framebuffer = framebuffersDynArraySmartPtr->begin()[m_SwapchainImageIx];
			beginInfo.renderpass = renderpassInitial;
			beginInfo.renderArea = area;
			beginInfo.clearValues = clear;
		}

		// you could do this later but only use renderpassInitial on first draw
		cb->beginRenderPass(&beginInfo, asset::ESC_INLINE);
		cb->endRenderPass();
	}

	void pipelineBarriersBeforeDraw(video::IGPUCommandBuffer* const cb)
	{
		auto& currentDrawBuffers = drawBuffers[m_resourceIx];
		{
			auto pseudoStencilImage = pseudoStencilImageView[m_resourceIx]->getCreationParameters().image;
			nbl::video::IGPUCommandBuffer::SImageMemoryBarrier imageBarriers[1u] = {};
			imageBarriers[0].barrier.srcAccessMask = nbl::asset::EAF_MEMORY_WRITE_BIT;
			imageBarriers[0].barrier.dstAccessMask = nbl::asset::EAF_SHADER_READ_BIT | nbl::asset::EAF_SHADER_WRITE_BIT; // SYNC_FRAGMENT_SHADER_SHADER_SAMPLED_READ | SYNC_FRAGMENT_SHADER_SHADER_STORAGE_READ | SYNC_FRAGMENT_SHADER_UNIFORM_READ
			imageBarriers[0].oldLayout = nbl::asset::IImage::EL_GENERAL;
			imageBarriers[0].newLayout = nbl::asset::IImage::EL_GENERAL;
			imageBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imageBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imageBarriers[0].image = pseudoStencilImage;
			imageBarriers[0].subresourceRange.aspectMask = nbl::asset::IImage::EAF_COLOR_BIT;
			imageBarriers[0].subresourceRange.baseMipLevel = 0u;
			imageBarriers[0].subresourceRange.levelCount = 1;
			imageBarriers[0].subresourceRange.baseArrayLayer = 0u;
			imageBarriers[0].subresourceRange.layerCount = 1;
			cb->pipelineBarrier(nbl::asset::EPSF_TRANSFER_BIT, nbl::asset::EPSF_FRAGMENT_SHADER_BIT, nbl::asset::EDF_NONE, 0u, nullptr, 0u, nullptr, 1u, imageBarriers);

		}
		{
			nbl::video::IGPUCommandBuffer::SBufferMemoryBarrier bufferBarriers[1u] = {};
			bufferBarriers[0u].barrier.srcAccessMask = nbl::asset::EAF_MEMORY_WRITE_BIT;
			bufferBarriers[0u].barrier.dstAccessMask = nbl::asset::EAF_INDEX_READ_BIT;
			bufferBarriers[0u].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bufferBarriers[0u].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bufferBarriers[0u].buffer = currentDrawBuffers.gpuDrawBuffers.indexBuffer;
			bufferBarriers[0u].offset = 0u;
			bufferBarriers[0u].size = currentDrawBuffers.getCurrentIndexBufferSize();
			cb->pipelineBarrier(nbl::asset::EPSF_TRANSFER_BIT, nbl::asset::EPSF_VERTEX_INPUT_BIT, nbl::asset::EDF_NONE, 0u, nullptr, 1u, bufferBarriers, 0u, nullptr);
		}
		{
			constexpr uint32_t MaxBufferBarriersCount = 5u;
			uint32_t bufferBarriersCount = 0u;
			nbl::video::IGPUCommandBuffer::SBufferMemoryBarrier bufferBarriers[MaxBufferBarriersCount] = {};

			if (globalsBuffer[m_resourceIx]->getSize() > 0u)
			{
				auto& bufferBarrier = bufferBarriers[bufferBarriersCount++];
				bufferBarrier.barrier.srcAccessMask = nbl::asset::EAF_MEMORY_WRITE_BIT;
				bufferBarrier.barrier.dstAccessMask = nbl::asset::EAF_UNIFORM_READ_BIT;
				bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				bufferBarrier.buffer = globalsBuffer[m_resourceIx];
				bufferBarrier.offset = 0u;
				bufferBarrier.size = globalsBuffer[m_resourceIx]->getSize();
			}
			if (currentDrawBuffers.getCurrentDrawObjectsBufferSize() > 0u)
			{
				auto& bufferBarrier = bufferBarriers[bufferBarriersCount++];
				bufferBarrier.barrier.srcAccessMask = nbl::asset::EAF_MEMORY_WRITE_BIT;
				bufferBarrier.barrier.dstAccessMask = nbl::asset::EAF_SHADER_READ_BIT;
				bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				bufferBarrier.buffer = currentDrawBuffers.gpuDrawBuffers.drawObjectsBuffer;
				bufferBarrier.offset = 0u;
				bufferBarrier.size = currentDrawBuffers.getCurrentDrawObjectsBufferSize();
			}
			if (currentDrawBuffers.getCurrentGeometryBufferSize() > 0u)
			{
				auto& bufferBarrier = bufferBarriers[bufferBarriersCount++];
				bufferBarrier.barrier.srcAccessMask = nbl::asset::EAF_MEMORY_WRITE_BIT;
				bufferBarrier.barrier.dstAccessMask = nbl::asset::EAF_SHADER_READ_BIT;
				bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				bufferBarrier.buffer = currentDrawBuffers.gpuDrawBuffers.geometryBuffer;
				bufferBarrier.offset = 0u;
				bufferBarrier.size = currentDrawBuffers.getCurrentGeometryBufferSize();
			}
			if (currentDrawBuffers.getCurrentLineStylesBufferSize() > 0u)
			{
				auto& bufferBarrier = bufferBarriers[bufferBarriersCount++];
				bufferBarrier.barrier.srcAccessMask = nbl::asset::EAF_MEMORY_WRITE_BIT;
				bufferBarrier.barrier.dstAccessMask = nbl::asset::EAF_SHADER_READ_BIT;
				bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				bufferBarrier.buffer = currentDrawBuffers.gpuDrawBuffers.lineStylesBuffer;
				bufferBarrier.offset = 0u;
				bufferBarrier.size = currentDrawBuffers.getCurrentLineStylesBufferSize();
			}
			if (currentDrawBuffers.getCurrentCustomClipProjectionBufferSize() > 0u)
			{
				auto& bufferBarrier = bufferBarriers[bufferBarriersCount++];
				bufferBarrier.barrier.srcAccessMask = nbl::asset::EAF_MEMORY_WRITE_BIT;
				bufferBarrier.barrier.dstAccessMask = nbl::asset::EAF_SHADER_READ_BIT;
				bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				bufferBarrier.buffer = currentDrawBuffers.gpuDrawBuffers.customClipProjectionBuffer;
				bufferBarrier.offset = 0u;
				bufferBarrier.size = currentDrawBuffers.getCurrentCustomClipProjectionBufferSize();
			}
			cb->pipelineBarrier(nbl::asset::EPSF_TRANSFER_BIT, nbl::asset::EPSF_VERTEX_SHADER_BIT | nbl::asset::EPSF_FRAGMENT_SHADER_BIT, nbl::asset::EDF_NONE, 0u, nullptr, bufferBarriersCount, bufferBarriers, 0u, nullptr);
		}
	}

	void endFrameRender()
	{
		auto& cb = m_cmdbuf[m_resourceIx];

		uint32_t windowWidth = swapchain->getCreationParameters().width;
		uint32_t windowHeight = swapchain->getCreationParameters().height;

		asset::SViewport vp;
		vp.minDepth = 1.f;
		vp.maxDepth = 0.f;
		vp.x = 0u;
		vp.y = 0u;
		vp.width = windowWidth;
		vp.height = windowHeight;
		cb->setViewport(0u, 1u, &vp);

		VkRect2D scissor;
		scissor.extent = { windowWidth, windowHeight };
		scissor.offset = { 0, 0 };
		cb->setScissor(0u, 1u, &scissor);

		nbl::video::IGPUCommandBuffer::SRenderpassBeginInfo beginInfo;
		{
			VkRect2D area;
			area.offset = { 0,0 };
			area.extent = { WIN_W, WIN_H };

			beginInfo.clearValueCount = 0u;
			beginInfo.framebuffer = framebuffersDynArraySmartPtr->begin()[m_SwapchainImageIx];
			beginInfo.renderpass = renderpassFinal;
			beginInfo.renderArea = area;
			beginInfo.clearValues = nullptr;
		}

		pipelineBarriersBeforeDraw(cb.get());

		cb->resetQueryPool(pipelineStatsPool.get(), 0u, 1u);
		cb->beginQuery(pipelineStatsPool.get(), 0);

		cb->beginRenderPass(&beginInfo, asset::ESC_INLINE);

		const uint32_t currentIndexCount = drawBuffers[m_resourceIx].getIndexCount();
		cb->bindDescriptorSets(asset::EPBP_GRAPHICS, graphicsPipelineLayout.get(), 0u, 1u, &descriptorSets[m_resourceIx].get());
		cb->bindIndexBuffer(drawBuffers[m_resourceIx].gpuDrawBuffers.indexBuffer.get(), 0u, asset::EIT_32BIT);
		cb->bindGraphicsPipeline(graphicsPipeline.get());
		cb->drawIndexed(currentIndexCount, 1u, 0u, 0u, 0u);

		if (fragmentShaderInterlockEnabled)
		{
			cb->bindDescriptorSets(asset::EPBP_GRAPHICS, resolveAlphaPipeLayout.get(), 0u, 1u, &descriptorSets[m_resourceIx].get());
			cb->bindGraphicsPipeline(resolveAlphaGraphicsPipeline.get());
			nbl::ext::FullScreenTriangle::recordDrawCalls(resolveAlphaGraphicsPipeline, 0u, swapchain->getPreTransform(), cb.get());
		}

		if constexpr (DebugMode)
		{
			cb->bindDescriptorSets(asset::EPBP_GRAPHICS, graphicsPipelineLayout.get(), 0u, 1u, &descriptorSets[m_resourceIx].get());
			cb->bindGraphicsPipeline(debugGraphicsPipeline.get());
			cb->drawIndexed(currentIndexCount, 1u, 0u, 0u, 0u);
		}
		cb->endQuery(pipelineStatsPool.get(), 0);
		cb->endRenderPass();

		cb->endDebugMarker();
		cb->end();

	}

	video::IGPUQueue::SSubmitInfo addObjects(video::IGPUQueue* submissionQueue, video::IGPUFence* submissionFence, video::IGPUQueue::SSubmitInfo& intendedNextSubmit)
	{
		// we record upload of our objects and if we failed to allocate we submit everything
		if (!intendedNextSubmit.isValid() || intendedNextSubmit.commandBufferCount <= 0u)
		{
			// log("intendedNextSubmit is invalid.", nbl::system::ILogger::ELL_ERROR);
			assert(false);
			return intendedNextSubmit;
		}

		// Use the last command buffer in intendedNextSubmit, it should be in recording state
		auto& cmdbuf = intendedNextSubmit.commandBuffers[intendedNextSubmit.commandBufferCount - 1];

		assert(cmdbuf->getState() == video::IGPUCommandBuffer::ES_RECORDING && cmdbuf->isResettable());
		assert(cmdbuf->getRecordingFlags().hasFlags(video::IGPUCommandBuffer::EU_ONE_TIME_SUBMIT_BIT));

		auto* cmdpool = cmdbuf->getPool();
		assert(cmdpool->getQueueFamilyIndex() == submissionQueue->getFamilyIndex());

		auto& currentDrawBuffers = drawBuffers[m_resourceIx];
		currentDrawBuffers.setSubmitDrawsFunction(
			[&](video::IGPUQueue* submissionQueue, video::IGPUFence* submissionFence, video::IGPUQueue::SSubmitInfo intendedNextSubmit)
			{
				return submitInBetweenDraws(m_resourceIx, submissionQueue, submissionFence, intendedNextSubmit);
			}
		);
		currentDrawBuffers.reset();

		if constexpr (mode == ExampleMode::CASE_0)
		{
			LineStyle style = {};
			style.screenSpaceLineWidth = 0.0f;
			style.worldSpaceLineWidth = 5.0f;
			style.color = float32_t4(0.7f, 0.3f, 0.1f, 0.5f);

			CPolyline polyline;
			{
				std::vector<float64_t2> linePoints;
				linePoints.push_back({ -50.0, -50.0 });
				linePoints.push_back({ 50.0, 50.0 });
				polyline.addLinePoints(core::SRange<float64_t2>(linePoints.data(), linePoints.data() + linePoints.size()));
			}

			intendedNextSubmit = currentDrawBuffers.drawPolyline(polyline, style, UseDefaultClipProjectionIdx, submissionQueue, submissionFence, intendedNextSubmit);
		}
		else if (mode == ExampleMode::CASE_1)
		{
			LineStyle style = {};
			style.screenSpaceLineWidth = 0.0f;
			style.worldSpaceLineWidth = 0.8f;
			style.color = float32_t4(0.619f, 0.325f, 0.709f, 0.2f);

			LineStyle style2 = {};
			style2.screenSpaceLineWidth = 0.0f;
			style2.worldSpaceLineWidth = 0.8f;
			style2.color = float32_t4(0.119f, 0.825f, 0.709f, 0.5f);

			intendedNextSubmit = currentDrawBuffers.drawPolyline(bigPolyline, style, UseDefaultClipProjectionIdx, submissionQueue, submissionFence, intendedNextSubmit);
			intendedNextSubmit = currentDrawBuffers.drawPolyline(bigPolyline2, style2, UseDefaultClipProjectionIdx, submissionQueue, submissionFence, intendedNextSubmit);
		}
		else if (mode == ExampleMode::CASE_2)
		{
		}
		else if (mode == ExampleMode::CASE_3)
		{
			LineStyle style = {};
			style.screenSpaceLineWidth = 4.0f;
			style.worldSpaceLineWidth = 0.0f;
			style.color = float32_t4(0.7f, 0.3f, 0.1f, 0.5f);

			LineStyle style2 = {};
			style2.screenSpaceLineWidth = 5.0f;
			style2.worldSpaceLineWidth = 0.0f;
			style2.color = float32_t4(0.2f, 0.6f, 0.2f, 0.5f);


			CPolyline polyline;
			CPolyline polyline2;
			
			{

				float Left = -100;
				float Right = 100;
				float Base = -25;
				srand(95);
				std::vector<QuadraticBezierInfo> quadBeziers;
				for (int i = 0; i < 10; i++) {
					QuadraticBezierInfo quadratic1;
					quadratic1.p[0] = float64_t2((rand() % 200 - 100), (rand() % 200 - 100));
					quadratic1.p[1] = float64_t2(0 + (rand() % 200 - 100), (rand() % 200 - 100));
					quadratic1.p[2] = float64_t2((rand() % 200 - 100), (rand() % 200 - 100));
					quadBeziers.push_back(quadratic1);
				}
				
				//{
				//	QuadraticBezierInfo quadratic1;
				//	quadratic1.p[0] = float64_t2(50,0);
				//	quadratic1.p[1] = float64_t2(50,100);
				//	quadratic1.p[2] = float64_t2(100,100);
				//	quadBeziers.push_back(quadratic1);
				//}
				//{
				//	QuadraticBezierInfo quadratic1;
				//	quadratic1.p[0] = float64_t2(100, 100);
				//	quadratic1.p[1] = float64_t2(200, -200);
				//	quadratic1.p[2] = float64_t2(300, 300);
				//	quadBeziers.push_back(quadratic1);
				//}
				polyline.addQuadBeziers(core::SRange<QuadraticBezierInfo>(quadBeziers.data(), quadBeziers.data() + quadBeziers.size()));

			}
			{
				std::vector<QuadraticBezierInfo> quadBeziers;
				{
					QuadraticBezierInfo quadratic1;
					quadratic1.p[0] = float64_t2(0.0, 0.0);
					quadratic1.p[1] = float64_t2(20.0, 50.0);
					quadratic1.p[2] = float64_t2(80.0, 0.0);
					//quadBeziers.push_back(quadratic1);
				}
				{
					QuadraticBezierInfo quadratic1;
					quadratic1.p[0] = float64_t2(80.0, 0.0);
					quadratic1.p[1] = float64_t2(220.0, 50.0);
					quadratic1.p[2] = float64_t2(180.0, 200.0);
					//quadBeziers.push_back(quadratic1);
				}
				{
					QuadraticBezierInfo quadratic1;
					quadratic1.p[0] = float64_t2(180.0, 200.0);
					quadratic1.p[1] = float64_t2(-20.0, 100.0);
					quadratic1.p[2] = float64_t2(30.0, -50.0);
					//quadBeziers.push_back(quadratic1);
				}

				// TODO: Test this after sdf fixes, cause a linear bezier is causing problems (I think)
				// MixedParabola myCurve = MixedParabola::fromFourPoints(float64_t2(-60.0, 90.0), float64_t2(0.0, 0.0), float64_t2(50.0, 0.0), float64_t2(60.0,-20.0));
				// error = 1e-2
				// 
				// ExplicitEllipse myCurve = ExplicitEllipse(20.0, 50.0);
				// MixedCircle myCurve = MixedCircle::fromFourPoints(float64_t2(90.0, 20.0), float64_t2(-50, 0.0), float64_t2(50.0, 0.0), float64_t2(60.0, 40.0));
				// Parabola myCurve = Parabola::fromThreePoints(float64_t2(-6.0, 4.0), float64_t2(0.0, 0.0), float64_t2(5.0, 0.0));
				MixedParabola myCurve = MixedParabola::fromFourPoints(float64_t2(-60.0, 90.0), float64_t2(0.0, 0.0), float64_t2(50.0, 0.0), float64_t2(60.0,-20.0));

				AddBezierFunc addToBezier = [&](const QuadraticBezierInfo& info) -> void
					{
						quadBeziers.push_back(info);
					};

				static int ix = 0;
				ix++;

				const int pp = (ix / 30) % 8;

				double error = pow(10.0, -1.0 * double(pp + 1));

				adaptiveSubdivision(myCurve, 0.0, 50.0, 1e-2, addToBezier, 10u);

				polyline2.addQuadBeziers(core::SRange<QuadraticBezierInfo>(quadBeziers.data(), quadBeziers.data() + quadBeziers.size()));
			}

			//intendedNextSubmit = currentDrawBuffers.drawPolyline(polyline, style, UseDefaultClipProjectionIdx, submissionQueue, submissionFence, intendedNextSubmit);
			intendedNextSubmit = currentDrawBuffers.drawPolyline(polyline2, style2, UseDefaultClipProjectionIdx, submissionQueue, submissionFence, intendedNextSubmit);

			ClipProjectionData customClipProject = {};
			customClipProject.projectionToNDC = m_Camera.constructViewProjection();
			customClipProject.projectionToNDC[0][0] *= 1.003f;
			customClipProject.projectionToNDC[1][1] *= 1.003f;
			customClipProject.maxClipNDC = float32_t2(0.5, 0.5);
			customClipProject.minClipNDC = float32_t2(-0.5, -0.5);
			uint32_t clipProjIdx = InvalidClipProjectionIdx;
			// intendedNextSubmit = currentDrawBuffers.addClipProjectionData_SubmitIfNeeded(customClipProject, clipProjIdx, submissionQueue, submissionFence, intendedNextSubmit);
			//intendedNextSubmit = currentDrawBuffers.drawPolyline(polyline, style2, clipProjIdx, submissionQueue, submissionFence, intendedNextSubmit);
		}
		intendedNextSubmit = currentDrawBuffers.finalizeAllCopiesToGPU(submissionQueue, submissionFence, intendedNextSubmit);
		return intendedNextSubmit;
	}

	video::IGPUQueue::SSubmitInfo submitInBetweenDraws(uint32_t resourceIdx, video::IGPUQueue* submissionQueue, video::IGPUFence* submissionFence, video::IGPUQueue::SSubmitInfo intendedNextSubmit)
	{
		// Use the last command buffer in intendedNextSubmit, it should be in recording state
		auto& cmdbuf = intendedNextSubmit.commandBuffers[intendedNextSubmit.commandBufferCount - 1];

		auto& currentDrawBuffers = drawBuffers[resourceIdx];

		uint32_t windowWidth = swapchain->getCreationParameters().width;
		uint32_t windowHeight = swapchain->getCreationParameters().height;

		nbl::video::IGPUCommandBuffer::SRenderpassBeginInfo beginInfo;
		{
			VkRect2D area;
			area.offset = { 0,0 };
			area.extent = { windowWidth, windowHeight };

			beginInfo.clearValueCount = 0u;
			beginInfo.framebuffer = framebuffersDynArraySmartPtr->begin()[m_SwapchainImageIx];
			beginInfo.renderpass = renderpassInBetween;
			beginInfo.renderArea = area;
			beginInfo.clearValues = nullptr;
		}

		asset::SViewport vp;
		vp.minDepth = 1.f;
		vp.maxDepth = 0.f;
		vp.x = 0u;
		vp.y = 0u;
		vp.width = windowWidth;
		vp.height = windowHeight;
		cmdbuf->setViewport(0u, 1u, &vp);

		VkRect2D scissor;
		scissor.extent = { windowWidth, windowHeight };
		scissor.offset = { 0, 0 };
		cmdbuf->setScissor(0u, 1u, &scissor);

		pipelineBarriersBeforeDraw(cmdbuf);

		cmdbuf->beginRenderPass(&beginInfo, asset::ESC_INLINE);

		const uint32_t currentIndexCount = drawBuffers[resourceIdx].getIndexCount();
		cmdbuf->bindDescriptorSets(asset::EPBP_GRAPHICS, graphicsPipelineLayout.get(), 0u, 1u, &descriptorSets[resourceIdx].get());
		cmdbuf->bindIndexBuffer(drawBuffers[resourceIdx].gpuDrawBuffers.indexBuffer.get(), 0u, asset::EIT_32BIT);
		cmdbuf->bindGraphicsPipeline(graphicsPipeline.get());
		cmdbuf->drawIndexed(currentIndexCount, 1u, 0u, 0u, 0u);

		if constexpr (DebugMode)
		{
			cmdbuf->bindGraphicsPipeline(debugGraphicsPipeline.get());
			cmdbuf->drawIndexed(currentIndexCount, 1u, 0u, 0u, 0u);
		}
		
		cmdbuf->endRenderPass();

		cmdbuf->end();

		video::IGPUQueue::SSubmitInfo submit = intendedNextSubmit;
		submit.signalSemaphoreCount = 0u;
		submit.pSignalSemaphores = nullptr;
		assert(submit.isValid());
		submissionQueue->submit(1u, &submit, submissionFence);
		intendedNextSubmit.commandBufferCount = 1u;
		intendedNextSubmit.commandBuffers = &cmdbuf;
		intendedNextSubmit.waitSemaphoreCount = 0u;
		intendedNextSubmit.pWaitSemaphores = nullptr;
		intendedNextSubmit.pWaitDstStageMask = nullptr;
		// we can reset the fence and commandbuffer because we fully wait for the GPU to finish here
		logicalDevice->blockForFences(1u, &submissionFence);
		logicalDevice->resetFences(1u, &submissionFence);
		cmdbuf->reset(video::IGPUCommandBuffer::ERF_RELEASE_RESOURCES_BIT);
		cmdbuf->begin(video::IGPUCommandBuffer::EU_ONE_TIME_SUBMIT_BIT);

		// reset things
		// currentDrawBuffers.clear();

		return intendedNextSubmit;
	}

	double dt = 0;
	double m_timeElapsed = 0.0;
	std::chrono::steady_clock::time_point lastTime;

	void workLoopBody() override
	{
		m_resourceIx++;
		if (m_resourceIx >= FRAMES_IN_FLIGHT)
			m_resourceIx = 0;

		auto now = std::chrono::high_resolution_clock::now();
		dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count();
		lastTime = now;
		m_timeElapsed += dt;

		if constexpr (mode == ExampleMode::CASE_0)
		{
			m_Camera.setSize(20.0 + abs(cos(m_timeElapsed * 0.001)) * 600);
		}

		inputSystem->getDefaultMouse(&mouse);
		inputSystem->getDefaultKeyboard(&keyboard);

		mouse.consumeEvents([&](const IMouseEventChannel::range_t& events) -> void
			{
				m_Camera.mouseProcess(events);
			}
		, logger.get());
		keyboard.consumeEvents([&](const IKeyboardEventChannel::range_t& events) -> void
			{
				m_Camera.keyboardProcess(events);
			}
		, logger.get());

		auto& cb = m_cmdbuf[m_resourceIx];
		auto& fence = m_frameComplete[m_resourceIx];

		auto& graphicsQueue = queues[CommonAPI::InitOutput::EQT_GRAPHICS];

		nbl::video::IGPUQueue::SSubmitInfo submit;
		submit.commandBufferCount = 1u;
		submit.commandBuffers = &cb.get();
		submit.signalSemaphoreCount = 1u;
		submit.pSignalSemaphores = &m_renderFinished[m_resourceIx].get();
		nbl::video::IGPUSemaphore* waitSemaphores[1u] = { m_imageAcquire[m_resourceIx].get() };
		asset::E_PIPELINE_STAGE_FLAGS waitStages[1u] = { nbl::asset::EPSF_COLOR_ATTACHMENT_OUTPUT_BIT };
		submit.waitSemaphoreCount = 1u;
		submit.pWaitSemaphores = waitSemaphores;
		submit.pWaitDstStageMask = waitStages;

		beginFrameRender();

		submit = addObjects(graphicsQueue, fence.get(), submit);

		endFrameRender();

		graphicsQueue->submit(1u, &submit, fence.get());

		CommonAPI::Present(
			logicalDevice.get(),
			swapchain.get(),
			queues[CommonAPI::InitOutput::EQT_GRAPHICS],
			m_renderFinished[m_resourceIx].get(),
			m_SwapchainImageIx);

		getAndLogQueryPoolResults();
	}

	bool keepRunning() override
	{
		return windowCb->isWindowOpen();
	}
};

//NBL_COMMON_API_MAIN(CADApp)
int main(int argc, char** argv) {
	CommonAPI::main<CADApp>(argc, argv);
}