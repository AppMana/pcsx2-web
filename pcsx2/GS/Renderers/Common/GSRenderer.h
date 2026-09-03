// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSState.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

class GSRenderer : public GSState
{
private:
	bool Merge(int field);
	bool BeginPresentFrame(bool frame_skip);
	void EndPresentFrame();

	u64 m_shader_time_start = 0;

	struct SnapshotDownload
	{
		GSTexture* rt = nullptr;
		std::unique_ptr<GSDownloadTexture> dl;
		u32 draw_width = 0;
		u32 draw_height = 0;
		u32 image_width = 0;
		u32 image_height = 0;
	};

	bool PrepareSnapshotDownload(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders, SnapshotDownload* sd);
	static void CopySnapshotPixels(const SnapshotDownload& sd, u32* width, u32* height, std::vector<u32>* pixels);

	std::string m_snapshot;
	u32 m_dump_frames = 0;
	u32 m_skipped_duplicate_frames = 0;

	// Tracking draw counters for idle frame detection.
	u64 m_last_draw_n = 0;
	u64 m_last_transfer_n = 0;

protected:
	GSVector2i m_real_size{0, 0};

	virtual GSTexture* GetOutput(int i, float& scale, int& y_offset) = 0;
	virtual GSTexture* GetFeedbackOutput(float& scale) { return nullptr; }

public:
	GSRenderer();
	virtual ~GSRenderer();

	virtual void Reset(bool hardware_reset) override;

	virtual void Destroy();

	virtual void UpdateRenderFixes();

	virtual void VSync(u32 field, bool registers_written, bool idle_frame);
	virtual bool CanUpscale() { return false; }
	virtual float GetUpscaleMultiplier() { return 1.0f; }
	virtual float GetTextureScaleFactor() { return 1.0f; }
	GSVector2i GetInternalResolution();
	float GetModXYOffset();

	virtual GSTexture* LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size);

	bool IsIdleFrame() const;

	bool SaveSnapshotToMemory(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
		u32* width, u32* height, std::vector<u32>* pixels);

	/// SaveSnapshotToMemory() for backends whose readbacks complete on an event loop: the frame is
	/// rendered and copied now, and the callback receives the pixels once the map finishes (an empty
	/// image if it failed). Returns false, without calling back, if nothing could be queued.
	using SnapshotCallback = std::function<void(u32 width, u32 height, std::vector<u32> pixels)>;
	bool SaveSnapshotToMemoryAsync(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
		SnapshotCallback callback);

	void QueueSnapshot(const std::string& path, const u32 gsdump_frames);
	void StopGSDump();
	void PresentCurrentFrame();
	bool BeginCapture(std::string filename, const GSVector2i& size = GSVector2i(0, 0));
	void EndCapture();
};

extern std::unique_ptr<GSRenderer> g_gs_renderer;
