/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

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

constexpr std::string_view OBSServiceFileName = "service.json";
constexpr std::string_view OBSService2FileName = "service2.json";
constexpr const char *kDefaultStream2Server = "srt://0.0.0.0:8001?mode=listener";

static void SaveServiceToFile(obs_service_t *svc, const std::filesystem::path &jsonFilePath, const char *label)
{
	if (!svc)
		return;

	OBSDataAutoRelease data = obs_data_create();
	OBSDataAutoRelease settings = obs_service_get_settings(svc);

	obs_data_set_string(data, "type", obs_service_get_type(svc));
	obs_data_set_obj(data, "settings", settings);

	if (!obs_data_save_json_safe(data, jsonFilePath.u8string().c_str(), "tmp", "bak")) {
		blog(LOG_WARNING, "Failed to save %s", label);
	}
}

void OBSBasic::SaveService()
{
	const OBSProfile &currentProfile = GetCurrentProfile();
	const std::filesystem::path jsonFilePath = currentProfile.path / std::filesystem::u8path(OBSServiceFileName);
	SaveServiceToFile(service, jsonFilePath, "service");
}

void OBSBasic::SaveService2()
{
	const OBSProfile &currentProfile = GetCurrentProfile();
	const std::filesystem::path jsonFilePath = currentProfile.path / std::filesystem::u8path(OBSService2FileName);
	SaveServiceToFile(service2, jsonFilePath, "service2");
}

bool OBSBasic::LoadService()
{
	OBSDataAutoRelease data;

	try {
		const OBSProfile &currentProfile = GetCurrentProfile();

		const std::filesystem::path jsonFilePath =
			currentProfile.path / std::filesystem::u8path(OBSServiceFileName);

		data = obs_data_create_from_json_file_safe(jsonFilePath.u8string().c_str(), "bak");

		if (!data) {
			return false;
		}
	} catch (const std::invalid_argument &error) {
		blog(LOG_ERROR, "%s", error.what());
		return false;
	}

	const char *type;
	obs_data_set_default_string(data, "type", "rtmp_common");
	type = obs_data_get_string(data, "type");

	OBSDataAutoRelease settings = obs_data_get_obj(data, "settings");
	OBSDataAutoRelease hotkey_data = obs_data_get_obj(data, "hotkeys");

	service = obs_service_create(type, "default_service", settings, hotkey_data);
	obs_service_release(service);

	if (!service) {
		return false;
	}

	/* Enforce Opus on WHIP if needed */
	if (strcmp(obs_service_get_protocol(service), "WHIP") == 0) {
		const char *option = config_get_string(activeConfiguration, "SimpleOutput", "StreamAudioEncoder");
		if (strcmp(option, "opus") != 0) {
			config_set_string(activeConfiguration, "SimpleOutput", "StreamAudioEncoder", "opus");
		}

		option = config_get_string(activeConfiguration, "AdvOut", "AudioEncoder");

		const char *encoder_codec = obs_get_encoder_codec(option);
		if (!encoder_codec || strcmp(encoder_codec, "opus") != 0) {
			config_set_string(activeConfiguration, "AdvOut", "AudioEncoder", "ffmpeg_opus");
		}
	}

	return true;
}

bool OBSBasic::LoadService2()
{
	OBSDataAutoRelease data;

	try {
		const OBSProfile &currentProfile = GetCurrentProfile();
		const std::filesystem::path jsonFilePath =
			currentProfile.path / std::filesystem::u8path(OBSService2FileName);

		data = obs_data_create_from_json_file_safe(jsonFilePath.u8string().c_str(), "bak");
		if (!data)
			return false;
	} catch (const std::invalid_argument &error) {
		blog(LOG_ERROR, "%s", error.what());
		return false;
	}

	obs_data_set_default_string(data, "type", "rtmp_custom");
	const char *type = obs_data_get_string(data, "type");
	OBSDataAutoRelease settings = obs_data_get_obj(data, "settings");
	OBSDataAutoRelease hotkey_data = obs_data_get_obj(data, "hotkeys");

	service2 = obs_service_create(type, "second_service", settings, hotkey_data);
	obs_service_release(service2);
	return service2 != nullptr;
}

bool OBSBasic::InitService()
{
	ProfileScope("OBSBasic::InitService");

	if (!LoadService()) {
		service = obs_service_create("rtmp_common", "default_service", nullptr, nullptr);
		if (!service)
			return false;
		obs_service_release(service);
	}

	if (!LoadService2()) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "server", kDefaultStream2Server);
		service2 = obs_service_create("rtmp_custom", "second_service", settings, nullptr);
		if (service2)
			obs_service_release(service2);
	}

	return true;
}

obs_service_t *OBSBasic::GetService()
{
	if (!service) {
		service = obs_service_create("rtmp_common", NULL, NULL, nullptr);
		obs_service_release(service);
	}
	return service;
}

void OBSBasic::SetService(obs_service_t *newService)
{
	if (newService) {
		service = newService;
	}
}

obs_service_t *OBSBasic::GetService2()
{
	if (!service2) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "server", kDefaultStream2Server);
		service2 = obs_service_create("rtmp_custom", "second_service", settings, nullptr);
		obs_service_release(service2);
	}
	return service2;
}

void OBSBasic::SetService2(obs_service_t *newService)
{
	if (newService)
		service2 = newService;
}

bool OBSBasic::Stream2Enabled()
{
	return config_get_bool(Config(), "Stream2", "Enable");
}
