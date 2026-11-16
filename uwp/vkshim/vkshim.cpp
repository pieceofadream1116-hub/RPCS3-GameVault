// Minimal stand-in for the Vulkan loader (vulkan-1.dll) on UWP: loads Mesa's Dozen
// driver from the package and forwards every vk* call to it. There is a single driver,
// so no layers or ICD discovery; instance and device functions are resolved through
// vkGetInstanceProcAddr once the instance exists (Mesa returns dispatching trampolines).
//
// VRAM clamp: the reported size of the GPU-local memory heaps is capped so RPCS3
// budgets its video memory more conservatively (attempt to avoid "out of video memory").

#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstring>
#include <vector>

extern "C"
{
	extern const char* const uwp_vk_names[];
	extern const size_t uwp_vk_count;
	extern void* uwp_vk_ptrs[];
	HMODULE WINAPI LoadPackagedLibrary(LPCWSTR name, DWORD reserved);

	// Set by the app to receive driver errors/warnings (Mesa reports its source file:line as the prefix).
	void (*uwp_vkshim_log)(const char* where, const char* text) = nullptr;
}

namespace
{
	PFN_vkGetInstanceProcAddr g_gipa = nullptr;

	// ---- VRAM clamp ----
	// Change this number to try other limits (in gigabytes).
	constexpr VkDeviceSize VRAM_LIMIT_GB = 2;
	constexpr VkDeviceSize VRAM_LIMIT = VRAM_LIMIT_GB * 1024ull * 1024ull * 1024ull;

	PFN_vkGetPhysicalDeviceMemoryProperties g_real_mem = nullptr;
	PFN_vkGetPhysicalDeviceMemoryProperties2 g_real_mem2 = nullptr;
	PFN_vkGetPhysicalDeviceMemoryProperties2 g_real_mem2_khr = nullptr;

	void clamp_heaps(VkPhysicalDeviceMemoryProperties& props)
	{
		for (uint32_t i = 0; i < props.memoryHeapCount; i++)
		{
			VkMemoryHeap& heap = props.memoryHeaps[i];
			if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && heap.size > VRAM_LIMIT)
				heap.size = VRAM_LIMIT;
		}
	}

	VKAPI_ATTR void VKAPI_CALL clamped_mem(VkPhysicalDevice dev, VkPhysicalDeviceMemoryProperties* props)
	{
		g_real_mem(dev, props);
		clamp_heaps(*props);
	}

	VKAPI_ATTR void VKAPI_CALL clamped_mem2(VkPhysicalDevice dev, VkPhysicalDeviceMemoryProperties2* props)
	{
		g_real_mem2(dev, props);
		clamp_heaps(props->memoryProperties);
	}

	VKAPI_ATTR void VKAPI_CALL clamped_mem2_khr(VkPhysicalDevice dev, VkPhysicalDeviceMemoryProperties2* props)
	{
		g_real_mem2_khr(dev, props);
		clamp_heaps(props->memoryProperties);
	}

	// Returns our wrapper for the memory query functions, or the driver's own function otherwise.
	PFN_vkVoidFunction wrap(const char* name, PFN_vkVoidFunction fn)
	{
		if (!fn || !name)
			return fn;

		if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0)
		{
			g_real_mem = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(fn);
			return reinterpret_cast<PFN_vkVoidFunction>(&clamped_mem);
		}
		if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties2") == 0)
		{
			g_real_mem2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(fn);
			return reinterpret_cast<PFN_vkVoidFunction>(&clamped_mem2);
		}
		if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0)
		{
			g_real_mem2_khr = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(fn);
			return reinterpret_cast<PFN_vkVoidFunction>(&clamped_mem2_khr);
		}
		return fn;
	}
	// ---- end VRAM clamp ----

	bool load_driver()
	{
		if (g_gipa)
			return true;

		HMODULE mod = LoadPackagedLibrary(L"vulkan_dzn.dll", 0);
		if (!mod)
			return false;

		g_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(mod, "vk_icdGetInstanceProcAddr"));
		return g_gipa != nullptr;
	}

	void resolve_all(VkInstance instance)
	{
		for (size_t i = 0; i < uwp_vk_count; i++)
		{
			if (auto fn = g_gipa(instance, uwp_vk_names[i]))
				uwp_vk_ptrs[i] = reinterpret_cast<void*>(wrap(uwp_vk_names[i], fn));
		}
	}

	template <typename F>
	F global_fn(const char* name)
	{
		return load_driver() ? reinterpret_cast<F>(g_gipa(nullptr, name)) : nullptr;
	}
}

extern "C"
{
	VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name)
	{
		return load_driver() ? wrap(name, g_gipa(instance, name)) : nullptr;
	}

	VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* layer, uint32_t* count, VkExtensionProperties* props)
	{
		const auto fn = global_fn<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties");
		return fn ? fn(layer, count, props) : VK_ERROR_INITIALIZATION_FAILED;
	}

	VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* count, VkLayerProperties*)
	{
		*count = 0; // No layers without the loader
		return VK_SUCCESS;
	}

	VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* version)
	{
		const auto fn = global_fn<PFN_vkEnumerateInstanceVersion>("vkEnumerateInstanceVersion");
		if (!fn)
		{
			*version = VK_API_VERSION_1_0;
			return VK_SUCCESS;
		}
		return fn(version);
	}

	VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* alloc, VkInstance* instance)
	{
		const auto fn = global_fn<PFN_vkCreateInstance>("vkCreateInstance");
		if (!fn)
			return VK_ERROR_INCOMPATIBLE_DRIVER;

		// Always enable VK_EXT_debug_report so driver errors reach the app log; there are no layers.
		std::vector<const char*> exts(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
		bool has_report = false;
		for (const char* e : exts)
			has_report |= std::strcmp(e, VK_EXT_DEBUG_REPORT_EXTENSION_NAME) == 0;
		if (!has_report)
			exts.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);

		VkInstanceCreateInfo patched = *info;
		patched.enabledLayerCount = 0;
		patched.ppEnabledLayerNames = nullptr;
		patched.enabledExtensionCount = static_cast<uint32_t>(exts.size());
		patched.ppEnabledExtensionNames = exts.data();

		const VkResult res = fn(&patched, alloc, instance);
		if (res != VK_SUCCESS)
			return res;

		resolve_all(*instance);

		VkDebugReportCallbackCreateInfoEXT cb{};
		cb.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
		cb.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
		cb.pfnCallback = [](VkDebugReportFlagsEXT, VkDebugReportObjectTypeEXT, uint64_t, size_t, int32_t, const char* prefix, const char* msg, void*) -> VkBool32
		{
			if (uwp_vkshim_log)
				uwp_vkshim_log(prefix ? prefix : "", msg ? msg : "");
			return VK_FALSE;
		};
		if (auto create = reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(g_gipa(*instance, "vkCreateDebugReportCallbackEXT")))
		{
			VkDebugReportCallbackEXT handle;
			create(*instance, &cb, nullptr, &handle); // Lives as long as the instance
		}
		return res;
	}
}
