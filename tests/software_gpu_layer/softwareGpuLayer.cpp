// A Vulkan layer for running the GPU regression tests on a software rasterizer.
//
// Mesa lavapipe implements nearly everything the renderer requires, but not depthBounds,
// VK_EXT_image_view_min_lod or VK_KHR_fragment_shader_barycentric. This layer reports the ones the
// driver below it lacks as available and removes them again before the driver sees them:
//
//   - depth bounds tests are never enabled,
//   - an image view's minimum LOD is dropped,
//   - fragment barycentrics stay unavailable to shaders.
//
// A test that depends on one of these therefore still fails; every other test runs. The emulator
// never loads the layer: it is enabled through VK_INSTANCE_LAYERS by tests/run_gpu_tests.sh only.
// A capability the driver does implement is passed through untouched.

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* LAYER_NAME = "VK_LAYER_KYTY_software_gpu_caps";

struct Missing {
	bool depth_bounds = false;
	bool min_lod      = false;
	bool barycentric  = false;
};

struct InstanceData {
	VkInstance                               instance  = VK_NULL_HANDLE;
	PFN_vkGetInstanceProcAddr                next_gipa = nullptr;
	PFN_vkDestroyInstance                    destroy_instance = nullptr;
	PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extensions = nullptr;
	PFN_vkGetPhysicalDeviceFeatures          get_features  = nullptr;
	PFN_vkGetPhysicalDeviceFeatures2         get_features2 = nullptr;
};

struct DeviceData {
	PFN_vkGetDeviceProcAddr           next_gdpa = nullptr;
	PFN_vkDestroyDevice               destroy_device = nullptr;
	PFN_vkCreateImageView             create_image_view = nullptr;
	PFN_vkCmdSetDepthBoundsTestEnable set_depth_bounds_test_enable = nullptr;
	Missing                           missing;
};

std::mutex                                g_mutex;
std::unordered_map<void*, InstanceData>   g_instances;
std::unordered_map<void*, DeviceData>     g_devices;

// Dispatchable handles start with the loader's dispatch table, which every object of an instance
// or of a device shares.
void* DispatchKey(const void* handle) {
	return *static_cast<void* const*>(handle);
}

InstanceData FindInstance(const void* handle) {
	const std::lock_guard lock(g_mutex);
	const auto            it = g_instances.find(DispatchKey(handle));
	return it != g_instances.end() ? it->second : InstanceData {};
}

DeviceData FindDevice(const void* handle) {
	const std::lock_guard lock(g_mutex);
	const auto            it = g_devices.find(DispatchKey(handle));
	return it != g_devices.end() ? it->second : DeviceData {};
}

std::vector<VkExtensionProperties> DriverExtensions(const InstanceData& instance,
                                                    VkPhysicalDevice    physical) {
	std::vector<VkExtensionProperties> extensions;
	VkResult                           result = VK_INCOMPLETE;
	while (result == VK_INCOMPLETE) {
		uint32_t count = 0;
		if (instance.enumerate_device_extensions(physical, nullptr, &count, nullptr) !=
		    VK_SUCCESS) {
			return {};
		}
		extensions.resize(count);
		result = instance.enumerate_device_extensions(physical, nullptr, &count, extensions.data());
		extensions.resize(count);
	}
	return result == VK_SUCCESS ? extensions : std::vector<VkExtensionProperties> {};
}

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
	return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
		return std::strcmp(extension.extensionName, name) == 0;
	});
}

Missing FindMissing(const InstanceData& instance, VkPhysicalDevice physical) {
	Missing                  missing;
	VkPhysicalDeviceFeatures features {};
	instance.get_features(physical, &features);
	missing.depth_bounds  = features.depthBounds != VK_TRUE;
	const auto extensions = DriverExtensions(instance, physical);
	missing.min_lod       = !HasExtension(extensions, VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
	missing.barycentric =
	    !HasExtension(extensions, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
	return missing;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures(VkPhysicalDevice          physical,
                                                     VkPhysicalDeviceFeatures* features) {
	const auto instance = FindInstance(physical);
	instance.get_features(physical, features);
	features->depthBounds = VK_TRUE;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice           physical,
                                                      VkPhysicalDeviceFeatures2* features) {
	const auto instance = FindInstance(physical);
	instance.get_features2(physical, features);
	const auto missing             = FindMissing(instance, physical);
	features->features.depthBounds = VK_TRUE;
	for (auto* next = static_cast<VkBaseOutStructure*>(features->pNext); next != nullptr;
	     next       = next->pNext) {
		if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT &&
		    missing.min_lod) {
			reinterpret_cast<VkPhysicalDeviceImageViewMinLodFeaturesEXT*>(next)->minLod = VK_TRUE;
		} else if (next->sType ==
		               VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR &&
		           missing.barycentric) {
			reinterpret_cast<VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR*>(next)
			    ->fragmentShaderBarycentric = VK_TRUE;
		}
	}
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical, const char* layer_name, uint32_t* count,
    VkExtensionProperties* properties) {
	if (layer_name != nullptr && std::strcmp(layer_name, LAYER_NAME) == 0) {
		*count = 0;
		return VK_SUCCESS;
	}
	const auto instance = FindInstance(physical);
	if (layer_name != nullptr) {
		return instance.enumerate_device_extensions(physical, layer_name, count, properties);
	}
	auto extensions = DriverExtensions(instance, physical);
	for (const auto& [name, version]:
	     {std::pair {VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME,
	                 uint32_t {VK_EXT_IMAGE_VIEW_MIN_LOD_SPEC_VERSION}},
	      std::pair {VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
	                 uint32_t {VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_SPEC_VERSION}}}) {
		if (!HasExtension(extensions, name)) {
			VkExtensionProperties extension {};
			std::strncpy(extension.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE - 1);
			extension.specVersion = version;
			extensions.push_back(extension);
		}
	}
	if (properties == nullptr) {
		*count = static_cast<uint32_t>(extensions.size());
		return VK_SUCCESS;
	}
	const auto copied = std::min<uint32_t>(*count, static_cast<uint32_t>(extensions.size()));
	std::copy_n(extensions.begin(), copied, properties);
	*count = copied;
	return copied < extensions.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device, const VkImageViewCreateInfo* info,
                                               const VkAllocationCallbacks* allocator,
                                               VkImageView*                 view) {
	const auto data = FindDevice(device);
	if (!data.missing.min_lod) {
		return data.create_image_view(device, info, allocator, view);
	}
	// Unlink the minimum LOD from the chain for the duration of the call.
	constexpr auto      min_lod_type = VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT;
	auto                copy         = *info;
	VkBaseOutStructure* previous     = nullptr;
	VkBaseOutStructure* removed      = nullptr;
	auto*               first = static_cast<VkBaseOutStructure*>(const_cast<void*>(copy.pNext));
	if (first != nullptr && first->sType == min_lod_type) {
		copy.pNext = first->pNext;
	} else {
		for (auto* next = first; next != nullptr && next->pNext != nullptr; next = next->pNext) {
			if (next->pNext->sType == min_lod_type) {
				previous       = next;
				removed        = next->pNext;
				previous->pNext = removed->pNext;
				break;
			}
		}
	}
	const auto result = data.create_image_view(device, &copy, allocator, view);
	if (previous != nullptr) {
		previous->pNext = removed;
	}
	return result;
}

VKAPI_ATTR void VKAPI_CALL CmdSetDepthBoundsTestEnable(VkCommandBuffer command, VkBool32 enable) {
	const auto data = FindDevice(command);
	data.set_depth_bounds_test_enable(command, data.missing.depth_bounds ? VK_FALSE : enable);
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
	if (device == VK_NULL_HANDLE) {
		return;
	}
	DeviceData data;
	{
		const std::lock_guard lock(g_mutex);
		const auto            it = g_devices.find(DispatchKey(device));
		data                     = it->second;
		g_devices.erase(it);
	}
	data.destroy_device(device, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice             physical,
                                            const VkDeviceCreateInfo*    create_info,
                                            const VkAllocationCallbacks* allocator,
                                            VkDevice*                    device) {
	auto* link = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(create_info->pNext));
	while (link != nullptr && (link->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
	                           link->function != VK_LAYER_LINK_INFO)) {
		link = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(link->pNext));
	}
	if (link == nullptr) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	const auto next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	const auto next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	link->u.pLayerInfo   = link->u.pLayerInfo->pNext;

	const auto instance = FindInstance(physical);
	const auto create =
	    reinterpret_cast<PFN_vkCreateDevice>(next_gipa(instance.instance, "vkCreateDevice"));
	if (create == nullptr) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	const auto missing = FindMissing(instance, physical);

	auto                     info = *create_info;
	std::vector<const char*> extensions;
	for (uint32_t i = 0; i < info.enabledExtensionCount; i++) {
		const char* name = info.ppEnabledExtensionNames[i];
		if ((missing.min_lod && std::strcmp(name, VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME) == 0) ||
		    (missing.barycentric &&
		     std::strcmp(name, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) == 0)) {
			continue;
		}
		extensions.push_back(name);
	}
	info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
	info.ppEnabledExtensionNames = extensions.data();

	VkPhysicalDeviceFeatures features {};
	if (info.pEnabledFeatures != nullptr) {
		features = *info.pEnabledFeatures;
		if (missing.depth_bounds) {
			features.depthBounds = VK_FALSE;
		}
		info.pEnabledFeatures = &features;
	}

	// Clear the features in the caller's chain for the duration of the call.
	std::vector<std::pair<VkBool32*, VkBool32>> restore;
	const auto clear = [&restore](VkBool32& feature) {
		restore.emplace_back(&feature, feature);
		feature = VK_FALSE;
	};
	for (auto* next = static_cast<VkBaseOutStructure*>(const_cast<void*>(info.pNext));
	     next != nullptr; next = next->pNext) {
		if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 && missing.depth_bounds) {
			clear(reinterpret_cast<VkPhysicalDeviceFeatures2*>(next)->features.depthBounds);
		} else if (next->sType ==
		               VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT &&
		           missing.min_lod) {
			clear(reinterpret_cast<VkPhysicalDeviceImageViewMinLodFeaturesEXT*>(next)->minLod);
		} else if (next->sType ==
		               VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR &&
		           missing.barycentric) {
			clear(reinterpret_cast<VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR*>(next)
			          ->fragmentShaderBarycentric);
		}
	}
	const auto result = create(physical, &info, allocator, device);
	for (const auto& [feature, value]: restore) {
		*feature = value;
	}
	if (result != VK_SUCCESS) {
		return result;
	}

	DeviceData data;
	data.next_gdpa      = next_gdpa;
	data.missing        = missing;
	data.destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(next_gdpa(*device, "vkDestroyDevice"));
	data.create_image_view =
	    reinterpret_cast<PFN_vkCreateImageView>(next_gdpa(*device, "vkCreateImageView"));
	data.set_depth_bounds_test_enable = reinterpret_cast<PFN_vkCmdSetDepthBoundsTestEnable>(
	    next_gdpa(*device, "vkCmdSetDepthBoundsTestEnable"));
	const std::lock_guard lock(g_mutex);
	g_devices[DispatchKey(*device)] = data;
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance                   instance,
                                           const VkAllocationCallbacks* allocator) {
	if (instance == VK_NULL_HANDLE) {
		return;
	}
	InstanceData data;
	{
		const std::lock_guard lock(g_mutex);
		const auto            it = g_instances.find(DispatchKey(instance));
		data                     = it->second;
		g_instances.erase(it);
	}
	data.destroy_instance(instance, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo*  create_info,
                                              const VkAllocationCallbacks* allocator,
                                              VkInstance*                  instance) {
	auto* link = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(create_info->pNext));
	while (link != nullptr && (link->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
	                           link->function != VK_LAYER_LINK_INFO)) {
		link = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(link->pNext));
	}
	if (link == nullptr) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	const auto next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	link->u.pLayerInfo   = link->u.pLayerInfo->pNext;
	const auto create =
	    reinterpret_cast<PFN_vkCreateInstance>(next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
	if (create == nullptr) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	const auto result = create(create_info, allocator, instance);
	if (result != VK_SUCCESS) {
		return result;
	}

	InstanceData data;
	data.instance  = *instance;
	data.next_gipa = next_gipa;
	data.destroy_instance =
	    reinterpret_cast<PFN_vkDestroyInstance>(next_gipa(*instance, "vkDestroyInstance"));
	data.enumerate_device_extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
	    next_gipa(*instance, "vkEnumerateDeviceExtensionProperties"));
	data.get_features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
	    next_gipa(*instance, "vkGetPhysicalDeviceFeatures"));
	data.get_features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
	    next_gipa(*instance, "vkGetPhysicalDeviceFeatures2"));
	const std::lock_guard lock(g_mutex);
	g_instances[DispatchKey(*instance)] = data;
	return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name);

PFN_vkVoidFunction InterceptedDeviceFunction(const char* name) {
	const std::pair<const char*, PFN_vkVoidFunction> functions[] = {
	    {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(GetDeviceProcAddr)},
	    {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice)},
	    {"vkCreateImageView", reinterpret_cast<PFN_vkVoidFunction>(CreateImageView)},
	    {"vkCmdSetDepthBoundsTestEnable",
	     reinterpret_cast<PFN_vkVoidFunction>(CmdSetDepthBoundsTestEnable)},
	    {"vkCmdSetDepthBoundsTestEnableEXT",
	     reinterpret_cast<PFN_vkVoidFunction>(CmdSetDepthBoundsTestEnable)},
	};
	for (const auto& [intercepted, function]: functions) {
		if (std::strcmp(name, intercepted) == 0) {
			return function;
		}
	}
	return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
	if (const auto function = InterceptedDeviceFunction(name)) {
		return function;
	}
	const auto data = FindDevice(device);
	return data.next_gdpa != nullptr ? data.next_gdpa(device, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance,
                                                             const char* name) {
	const std::pair<const char*, PFN_vkVoidFunction> functions[] = {
	    {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(GetInstanceProcAddr)},
	    {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(CreateInstance)},
	    {"vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance)},
	    {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(CreateDevice)},
	    {"vkEnumerateDeviceExtensionProperties",
	     reinterpret_cast<PFN_vkVoidFunction>(EnumerateDeviceExtensionProperties)},
	    {"vkGetPhysicalDeviceFeatures",
	     reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceFeatures)},
	    {"vkGetPhysicalDeviceFeatures2",
	     reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceFeatures2)},
	    {"vkGetPhysicalDeviceFeatures2KHR",
	     reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceFeatures2)},
	};
	for (const auto& [intercepted, function]: functions) {
		if (std::strcmp(name, intercepted) == 0) {
			return function;
		}
	}
	if (const auto function = InterceptedDeviceFunction(name)) {
		return function;
	}
	if (instance == VK_NULL_HANDLE) {
		return nullptr;
	}
	const auto data = FindInstance(instance);
	return data.next_gipa != nullptr ? data.next_gipa(instance, name) : nullptr;
}

} // namespace

extern "C" __attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* interface) {
	if (interface == nullptr || interface->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	interface->loaderLayerInterfaceVersion =
	    std::min<uint32_t>(interface->loaderLayerInterfaceVersion, 2);
	interface->pfnGetInstanceProcAddr       = GetInstanceProcAddr;
	interface->pfnGetDeviceProcAddr         = GetDeviceProcAddr;
	interface->pfnGetPhysicalDeviceProcAddr = nullptr;
	return VK_SUCCESS;
}
