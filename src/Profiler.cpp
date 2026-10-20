#include "Profiler.h"

#include <algorithm>
#include <unordered_map>

float Profiler::RollingHistory::GetAverage() const
{
	if (count == 0)
		return lastMs;
	float sum = 0.0f;
	for (uint32_t i = 0; i < count; i++)
		sum += history[i];
	return sum / static_cast<float>(count);
}

float Profiler::RollingHistory::GetPercentile(float p) const
{
	if (count == 0)
		return lastMs;

	thread_local std::vector<float> sorted;
	sorted.resize(count);
	for (uint32_t i = 0; i < count; i++)
		sorted[i] = history[i];
	std::sort(sorted.begin(), sorted.end());

	float idx = (p / 100.0f) * static_cast<float>(count - 1);
	uint32_t lo = static_cast<uint32_t>(idx);
	uint32_t hi = std::min(lo + 1, count - 1);
	float frac = idx - static_cast<float>(lo);
	return sorted[lo] * (1.0f - frac) + sorted[hi] * frac;
}

void Profiler::RollingHistory::GetPercentiles95And99(float& a_p95, float& a_p99) const
{
	if (count == 0) {
		a_p95 = lastMs;
		a_p99 = lastMs;
		return;
	}

	thread_local std::vector<float> values;
	values.assign(history, history + count);

	// The same lo/hi/frac as GetPercentile.
	const auto bounds = [this](float p) {
		const float idx = (p / 100.0f) * static_cast<float>(count - 1);
		const uint32_t lo = static_cast<uint32_t>(idx);
		return std::tuple{ lo, std::min(lo + 1, count - 1), idx - static_cast<float>(lo) };
	};
	const auto [lo95, hi95, frac95] = bounds(95.0f);
	const auto [lo99, hi99, frac99] = bounds(99.0f);

	// nth_element puts the lo-th smallest in place with everything above it to its
	// right, so the next order statistic is the minimum of that remainder.
	const auto first = values.begin();
	std::nth_element(first, first + lo95, values.end());
	const float s95lo = values[lo95];
	const float s95hi = hi95 == lo95 ? s95lo : *std::min_element(first + lo95 + 1, values.end());
	if (lo99 > lo95)
		std::nth_element(first + lo95 + 1, first + lo99, values.end());
	const float s99lo = values[lo99];
	const float s99hi = hi99 == lo99 ? s99lo : *std::min_element(first + lo99 + 1, values.end());

	a_p95 = s95lo * (1.0f - frac95) + s95hi * frac95;
	a_p99 = s99lo * (1.0f - frac99) + s99hi * frac99;
}

void Profiler::Initialize(ID3D11Device* device, ID3D11DeviceContext* a_context)
{
	Release();

	context = a_context;

	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	cpuTicksToMs = 1000.0 / static_cast<double>(freq.QuadPart);

	for (auto& frame : frames) {
		D3D11_QUERY_DESC disjointDesc{};
		disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		device->CreateQuery(&disjointDesc, frame.disjoint.put());

		frame.timers.resize(kMaxTimers);
		for (auto& timer : frame.timers) {
			D3D11_QUERY_DESC tsDesc{};
			tsDesc.Query = D3D11_QUERY_TIMESTAMP;
			device->CreateQuery(&tsDesc, timer.begin.put());
			device->CreateQuery(&tsDesc, timer.end.put());
		}
		frame.activeCount = 0;
		frame.inFlight = false;
	}

	writeFrame = 0;
	readFrame = 0;
	framesSinceInit = 0;
	initialized = true;
}

void Profiler::Release()
{
	for (auto& frame : frames) {
		frame.disjoint = nullptr;
		frame.timers.clear();
		frame.skipped.clear();
		frame.activeCount = 0;
		frame.inFlight = false;
	}
	pendingSkipped.clear();
	results.clear();
	knownTimers.clear();
	knownTimerIndex.clear();
	collectedFrames = 0;
	totalTimeMs = 0.0f;
	cpuTotalTimeMs = 0.0f;
	initialized = false;
	context = nullptr;
}

void Profiler::BeginFrame()
{
	if (!initialized || !context || frameActive)
		return;

	CollectResults();

	auto& frame = frames[writeFrame];
	frame.activeCount = 0;
	frame.skipped.clear();
	frame.skipped.swap(pendingSkipped);
	frame.inFlight = true;
	frameActive = true;
	context->Begin(frame.disjoint.get());
}

void Profiler::BeginPass(const std::string& name)
{
	if (!initialized || !context)
		return;

	if (!frameActive)
		BeginFrame();

	auto& frame = frames[writeFrame];
	if (frame.activeCount >= kMaxTimers)
		return;

	auto& timer = frame.timers[frame.activeCount];
	timer.name = name;
	context->End(timer.begin.get());
	QueryPerformanceCounter(&timer.cpuBegin);

	if (beginPerfEvent)
		beginPerfEvent(name);
}

void Profiler::EndPass()
{
	if (!initialized || !context || !frameActive)
		return;

	auto& frame = frames[writeFrame];
	if (frame.activeCount >= kMaxTimers)
		return;

	auto& timer = frame.timers[frame.activeCount];

	LARGE_INTEGER cpuEnd;
	QueryPerformanceCounter(&cpuEnd);
	timer.cpuMs = static_cast<float>(static_cast<double>(cpuEnd.QuadPart - timer.cpuBegin.QuadPart) * cpuTicksToMs);

	context->End(timer.end.get());
	frame.activeCount++;

	if (endPerfEvent)
		endPerfEvent({});
}

void Profiler::MarkPassSkipped(const std::string& name)
{
	if (!initialized)
		return;

	// Resolved when the frame is collected, against the passes that ran in it.
	(frameActive ? frames[writeFrame].skipped : pendingSkipped).push_back(name);
}

Profiler::KnownTimer& Profiler::FindOrAddTimer(const std::string& name)
{
	auto [it, inserted] = knownTimerIndex.try_emplace(name, knownTimers.size());
	if (inserted) {
		KnownTimer kt;
		kt.name = name;
		knownTimers.push_back(std::move(kt));
	}
	return knownTimers[it->second];
}

void Profiler::EndFrame()
{
	if (!initialized || !context || !frameActive)
		return;

	frameActive = false;
	context->End(frames[writeFrame].disjoint.get());
	writeFrame = (writeFrame + 1) % kFrameLatency;
	framesSinceInit++;
}

void Profiler::CollectResults()
{
	if (framesSinceInit < kFrameLatency)
		return;

	readFrame = writeFrame;
	auto& frame = frames[readFrame];
	if (!frame.inFlight)
		return;

	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
	HRESULT hr = context->GetData(frame.disjoint.get(), &disjointData, sizeof(disjointData), D3D11_ASYNC_GETDATA_DONOTFLUSH);
	if (hr != S_OK)
		return;

	frame.inFlight = false;
	collectedFrames++;

	struct ActiveTimerData
	{
		float gpuMs = 0.0f;
		float cpuMs = 0.0f;
	};
	std::unordered_map<std::string, ActiveTimerData> activeTimers;
	std::vector<const std::string*> activeOrder;
	float activeTotalMs = 0.0f;
	float activeCpuTotalMs = 0.0f;

	if (!disjointData.Disjoint) {
		double ticksToMs = 1000.0 / static_cast<double>(disjointData.Frequency);

		for (uint32_t i = 0; i < frame.activeCount; i++) {
			auto& timer = frame.timers[i];
			UINT64 tsBegin = 0, tsEnd = 0;

			if (context->GetData(timer.begin.get(), &tsBegin, sizeof(tsBegin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				continue;
			if (context->GetData(timer.end.get(), &tsEnd, sizeof(tsEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				continue;

			float ms = static_cast<float>(static_cast<double>(tsEnd - tsBegin) * ticksToMs);
			auto [entry, first] = activeTimers.try_emplace(timer.name);
			if (first)
				activeOrder.push_back(&entry->first);
			entry->second.gpuMs += ms;
			entry->second.cpuMs += timer.cpuMs;
			activeTotalMs += ms;
			activeCpuTotalMs += timer.cpuMs;
		}

		// One sample per name per frame, so a row timed in several places reads as their sum.
		for (const std::string* name : activeOrder) {
			const auto& sums = activeTimers[*name];
			auto& known = FindOrAddTimer(*name);
			known.gpu.PushSample(sums.gpuMs);
			known.cpu.PushSample(sums.cpuMs);
			known.lastSampleFrame = collectedFrames;
		}
		for (const auto& name : frame.skipped) {
			if (activeTimers.contains(name))
				continue;
			auto& known = FindOrAddTimer(name);
			if (known.lastSampleFrame == collectedFrames)
				continue;
			known.gpu.PushSample(0.0f);
			known.cpu.PushSample(0.0f);
			known.lastSampleFrame = collectedFrames;
		}
	}

	RetireStaleTimers();

	totalTimeMs = activeTotalMs;
	cpuTotalTimeMs = activeCpuTotalMs;

	results.clear();
	results.reserve(knownTimers.size());
	for (const auto& known : knownTimers) {
		TimerResult result;
		result.name = known.name;
		auto it = activeTimers.find(known.name);
		if (it != activeTimers.end()) {
			result.gpuTimeMs = it->second.gpuMs;
			result.cpuTimeMs = it->second.cpuMs;
		} else {
			result.gpuTimeMs = known.gpu.lastMs;
			result.cpuTimeMs = known.cpu.lastMs;
		}
		result.avgMs = known.gpu.GetAverage();
		known.gpu.GetPercentiles95And99(result.p95Ms, result.p99Ms);
		result.cpuAvgMs = known.cpu.GetAverage();
		known.cpu.GetPercentiles95And99(result.cpuP95Ms, result.cpuP99Ms);
		result.valid = true;
		result.historyBuffer = known.gpu.history;
		result.historyHead = known.gpu.head;
		result.historyCount = known.gpu.count;
		results.push_back(std::move(result));
	}
}
void Profiler::RetireStaleTimers()
{
	const size_t before = knownTimers.size();
	std::erase_if(knownTimers, [this](const KnownTimer& known) {
		return collectedFrames - known.lastSampleFrame >= kTimerRetireFrames;
	});
	if (knownTimers.size() != before)
		RebuildTimerIndex();
}

void Profiler::RebuildTimerIndex()
{
	knownTimerIndex.clear();
	for (size_t i = 0; i < knownTimers.size(); i++)
		knownTimerIndex[knownTimers[i].name] = i;
}
