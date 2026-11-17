/******************************************************************************
    Copyright (C) 2025 by Dennis Sädtler <saedtler@twitch.tv>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <qt-wrappers.hpp>

#include <QHBoxLayout>
#include <QLabel>

namespace {
constexpr const char *kSecondScreenCanvasName = "Second Screen";
}

void OBSBasic::CanvasRemoved(void *data, calldata_t *params)
{
	OBSCanvas canvas = static_cast<obs_canvas_t *>(calldata_ptr(params, "canvas"));
	QMetaObject::invokeMethod(static_cast<OBSBasic *>(data), &OBSBasic::RemoveCanvas, canvas);
}

const OBS::Canvas &OBSBasic::AddCanvas(const std::string &name, obs_video_info *ovi, int flags)
{
	OBSCanvas canvas = obs_canvas_create(name.c_str(), ovi, flags);
	auto &it = canvases.emplace_back(canvas);
	OnEvent(OBS_FRONTEND_EVENT_CANVAS_ADDED);
	UpdateCanvasSelector();
	return it;
}

bool OBSBasic::RemoveCanvas(OBSCanvas canvas)
{
	bool removed = false;
	if (!canvas) {
		return removed;
	}

	auto canvas_it = std::find(std::begin(canvases), std::end(canvases), canvas);
	if (canvas_it != std::end(canvases)) {
		OBS::Canvas tmp = std::move(*canvas_it);
		canvases.erase(canvas_it);
		removed = true;
	}

	if (removed) {
		OnEvent(OBS_FRONTEND_EVENT_CANVAS_REMOVED);
		UpdateCanvasSelector();
	}

	return removed;
}

void OBSBasic::ClearCanvases()
{
	while (!canvases.empty()) {
		RemoveCanvas(OBSCanvas(canvases.back()));
	}
}

OBSCanvasAutoRelease OBSBasic::GetEditingCanvas()
{
	obs_canvas_t *canvas = obs_weak_canvas_get_canvas(editingCanvas);
	if (canvas)
		return canvas;
	return obs_get_main_canvas();
}

bool OBSBasic::IsEditingMainCanvas()
{
	OBSCanvasAutoRelease editing = GetEditingCanvas();
	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
	return editing.Get() == mainCanvas.Get();
}

bool OBSBasic::SourceBelongsToEditingCanvas(obs_source_t *source)
{
	if (!source)
		return false;

	OBSCanvasAutoRelease editing = GetEditingCanvas();
	OBSCanvasAutoRelease sourceCanvas = obs_source_get_canvas(source);
	return sourceCanvas && editing && sourceCanvas.Get() == editing.Get();
}

static bool canvas_has_any_scene(obs_canvas_t *canvas)
{
	bool found = false;
	obs_canvas_enum_scenes(
		canvas,
		[](void *param, obs_source_t *) {
			*static_cast<bool *>(param) = true;
			return false;
		},
		&found);
	return found;
}

static void ensure_canvas_video(obs_canvas_t *canvas)
{
	obs_video_info ovi;
	if (!canvas || !obs_get_video_info(&ovi))
		return;

	/* Scene-collection load creates extra canvases without a video mix. */
	if (!obs_canvas_has_video(canvas) || !obs_canvas_get_video(canvas)) {
		if (!obs_canvas_reset_video(canvas, &ovi))
			blog(LOG_WARNING, "Failed to reset video for canvas '%s'", obs_canvas_get_name(canvas));
	}
}

obs_canvas_t *OBSBasic::EnsureSecondScreenCanvas()
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		blog(LOG_WARNING, "Could not create Second Screen canvas: no video info");
		return nullptr;
	}

	for (const auto &canvas : canvases) {
		if (strcmp(obs_canvas_get_name(canvas), kSecondScreenCanvasName) != 0)
			continue;

		ensure_canvas_video(canvas);

		if (!canvas_has_any_scene(canvas)) {
			std::string sceneName = QT_TO_UTF8(QTStr("Basic.Main.DefaultSceneName.Text").arg(2));
			OBSSourceAutoRelease existing = obs_get_source_by_name(sceneName.c_str());
			int n = 3;
			while (existing) {
				existing = nullptr;
				sceneName = QT_TO_UTF8(QTStr("Basic.Main.DefaultSceneName.Text").arg(n++));
				existing = obs_get_source_by_name(sceneName.c_str());
			}

			OBSSceneAutoRelease scene = obs_canvas_scene_create(canvas, sceneName.c_str());
			if (scene)
				obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
		}

		return canvas;
	}

	/* ACTIVATE + SCENE_REF so the mix runs; skip MIX_AUDIO so desktop audio is not doubled. */
	const OBS::Canvas &added = AddCanvas(kSecondScreenCanvasName, &ovi, ACTIVATE | SCENE_REF);
	obs_canvas_t *canvas = added;

	std::string sceneName = QT_TO_UTF8(QTStr("Basic.Main.DefaultSceneName.Text").arg(2));
	OBSSourceAutoRelease existing = obs_get_source_by_name(sceneName.c_str());
	int n = 3;
	while (existing) {
		existing = nullptr;
		sceneName = QT_TO_UTF8(QTStr("Basic.Main.DefaultSceneName.Text").arg(n++));
		existing = obs_get_source_by_name(sceneName.c_str());
	}

	OBSSceneAutoRelease scene = obs_canvas_scene_create(canvas, sceneName.c_str());
	if (scene) {
		obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
		blog(LOG_INFO, "Created Second Screen canvas with scene '%s'", sceneName.c_str());
	}
	return canvas;
}

void OBSBasic::SetEditingCanvas(obs_canvas_t *canvas)
{
	if (!canvas)
		return;

	OBSCanvasAutoRelease current = GetEditingCanvas();
	if (current.Get() == canvas)
		return;

	ensure_canvas_video(canvas);

	OBSSource cur = GetCurrentSceneSource();
	if (cur && current) {
		currentScenePerCanvas[obs_canvas_get_uuid(current)] = OBSGetWeakRef(cur);
	}

	editingCanvas = OBSGetWeakRef(canvas);
	RefreshScenesForEditingCanvas();

	obs_video_info ovi;
	if (obs_canvas_get_video_info(canvas, &ovi)) {
		emit CanvasResized(ovi.base_width, ovi.base_height);
		ResizePreview(ovi.base_width, ovi.base_height);
	}

	UpdateCanvasSelector();
}

void OBSBasic::SyncCanvasScenesToUI()
{
	auto add_missing = [this](obs_source_t *source) {
		if (obs_scene_from_source(source))
			AddScene(source);
		return true;
	};

	for (const auto &canvas : canvases) {
		obs_canvas_enum_scenes(
			canvas,
			[](void *param, obs_source_t *source) {
				return (*static_cast<decltype(add_missing) *>(param))(source);
			},
			&add_missing);
	}
}

void OBSBasic::RefreshScenesForEditingCanvas()
{
	OBSCanvasAutoRelease editing = GetEditingCanvas();
	if (!editing)
		return;

	SyncCanvasScenesToUI();

	auto find_select = [&]() -> QListWidgetItem * {
		OBSWeakSource saved;
		auto it = currentScenePerCanvas.find(obs_canvas_get_uuid(editing));
		if (it != currentScenePerCanvas.end())
			saved = it->second;

		OBSSourceAutoRelease preferred = OBSGetStrongRef(saved);
		QListWidgetItem *select = nullptr;

		for (int i = 0; i < ui->scenes->count(); i++) {
			QListWidgetItem *item = ui->scenes->item(i);
			OBSScene scene = GetOBSRef<OBSScene>(item);
			obs_source_t *source = obs_scene_get_source(scene);
			OBSCanvasAutoRelease sourceCanvas = obs_source_get_canvas(source);
			const bool onCanvas = sourceCanvas && sourceCanvas.Get() == editing.Get();
			item->setHidden(!onCanvas);
			if (!onCanvas)
				continue;

			if (preferred && source == preferred.Get()) {
				select = item;
				break;
			}
			if (!select)
				select = item;
		}
		return select;
	};

	QListWidgetItem *select = find_select();
	if (!select) {
		OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
		if (editing.Get() != mainCanvas.Get()) {
			EnsureSecondScreenCanvas();
			SyncCanvasScenesToUI();
			select = find_select();
		}
	}

	ui->scenes->blockSignals(true);
	ui->scenes->setCurrentItem(select);
	ui->scenes->blockSignals(false);

	OBSSource nextSource;
	if (select) {
		OBSScene scene = GetOBSRef<OBSScene>(select);
		nextSource = obs_scene_get_source(scene);
		currentScene = scene.Get();
	} else {
		currentScene = nullptr;
	}

	ApplyEditingCanvasScene(nextSource, true);
	OnEvent(OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED);
	UpdateContextBar();
}

void OBSBasic::ApplyEditingCanvasScene(OBSSource scene, bool force)
{
	OBSCanvasAutoRelease editing = GetEditingCanvas();
	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();

	if (!editing)
		return;

	if (editing.Get() == mainCanvas.Get()) {
		if (!IsPreviewProgramMode()) {
			TransitionToScene(scene, force);
		} else if (scene) {
			OBSSource actualLastScene = OBSGetStrongRef(lastScene);
			if (actualLastScene != scene) {
				obs_source_inc_showing(scene);
				if (actualLastScene)
					obs_source_dec_showing(actualLastScene);
				lastScene = OBSGetWeakRef(scene);
			}
		}
		return;
	}

	/* Secondary canvas: drive that canvas's channel 0 for encode + preview. */
	obs_canvas_set_channel(editing, 0, scene);
}

obs_source_t *OBSBasic::CreateSourceOnEditingCanvas(const char *id, const char *name, obs_data_t *settings)
{
	OBSCanvasAutoRelease editing = GetEditingCanvas();
	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
	const char *v_id = obs_get_latest_input_type_id(id);

	if (editing && mainCanvas && editing.Get() != mainCanvas.Get())
		return obs_source_create_canvas(editing, v_id, name, settings, nullptr);

	return obs_source_create(v_id, name, settings, nullptr);
}

void OBSBasic::InitCanvasSwitcher()
{
	if (canvasSelector)
		return;

	auto *row = new QWidget(ui->previewContainer);
	auto *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(8);

	auto *label = new QLabel(QTStr("Basic.Main.EditingCanvas"), row);
	canvasSelector = new QComboBox(row);
	canvasSelector->setMinimumWidth(160);
	layout->addWidget(label);
	layout->addWidget(canvasSelector, 1);

	ui->previewTextLayout->insertWidget(0, row);

	connect(canvasSelector, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&OBSBasic::CanvasSelectorChanged);

	UpdateCanvasSelector();
}

void OBSBasic::UpdateCanvasSelector()
{
	if (!canvasSelector)
		return;

	OBSCanvasAutoRelease editing = GetEditingCanvas();
	const QSignalBlocker blocker(canvasSelector);
	canvasSelector->clear();

	OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
	canvasSelector->addItem(QTStr("Basic.Main.MainCanvas"), QString::fromUtf8(obs_canvas_get_uuid(mainCanvas)));

	for (const auto &canvas : canvases) {
		if (obs_canvas_get_flags(canvas) & EPHEMERAL)
			continue;
		canvasSelector->addItem(QString::fromUtf8(obs_canvas_get_name(canvas)),
					QString::fromUtf8(obs_canvas_get_uuid(canvas)));
	}

	int idx = canvasSelector->findData(QString::fromUtf8(obs_canvas_get_uuid(editing)));
	if (idx < 0)
		idx = 0;
	canvasSelector->setCurrentIndex(idx);
}

void OBSBasic::CanvasSelectorChanged(int index)
{
	if (index < 0 || !canvasSelector)
		return;

	const QString uuid = canvasSelector->itemData(index).toString();
	OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(QT_TO_UTF8(uuid));
	if (!canvas)
		canvas = obs_get_main_canvas();
	SetEditingCanvas(canvas);
}
