/* Bounded EGL draw/readback and Vulkan transfer smoke; not conformance. */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int software(const char *s)
{
    return !s || strstr(s, "llvmpipe") || strstr(s, "softpipe") ||
           strstr(s, "SwiftShader") || strstr(s, "swiftshader") ||
           strstr(s, "lavapipe") || strstr(s, "Software");
}

static int egl_probe(void)
{
    int ok = 0;
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLConfig config;
    EGLint count = 0, major = 0, minor = 0;
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) goto out;
    printf("EGL wrapper: vendor=%s version=%s (metadata only)\n",
           eglQueryString(d, EGL_VENDOR), eglQueryString(d, EGL_VERSION));
    if (!eglGetConfigs(d, NULL, 0, &count) || count == 0) goto out;
    printf("EGL configs=%d\n", count);
    if (!eglChooseConfig(d, attrs, &config, 1, &count) || count != 1 ||
        !eglBindAPI(EGL_OPENGL_ES_API)) goto out;
    surface = eglCreatePbufferSurface(d, config, size);
    context = eglCreateContext(d, config, EGL_NO_CONTEXT, version);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(d, surface, surface, context)) goto out;
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *gl_version = (const char *)glGetString(GL_VERSION);
    printf("GLES vendor=%s renderer=%s version=%s\n",
           vendor ? vendor : "NULL", renderer ? renderer : "NULL",
           gl_version ? gl_version : "NULL");
    if (!vendor || !gl_version || software(renderer) ||
        (!strstr(renderer, "AMD") && !strstr(renderer, "GFX1013"))) goto out;
    GLubyte pixel[4] = {0};
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("GLES readback=%02x%02x%02x%02x expected=ff00ffff\n",
           pixel[0], pixel[1], pixel[2], pixel[3]);
    ok = glGetError() == GL_NO_ERROR && pixel[0] == 255 && pixel[1] == 0 &&
         pixel[2] == 255 && pixel[3] == 255;
out:
    if (!ok) printf("EGL/GLES failed, EGL error=0x%x\n", eglGetError());
    if (d != EGL_NO_DISPLAY) {
        eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(d, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(d, surface);
        eglTerminate(d);
    }
    return ok;
}

#define VK_CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { \
    printf("%s failed: %d\n", #call, r); goto out; } } while (0)

static int vk_probe(void)
{
    int ok = 0, submitted = 0, completed = 0;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void *mapped = NULL;
    VkPhysicalDevice *devices = NULL;
    VkQueueFamilyProperties *families = NULL;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "redroid-gpu-smoke",
                            .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app};
    VK_CHECK(vkCreateInstance(&ici, NULL, &instance));
    uint32_t count = 0, family = UINT32_MAX;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) goto out;
    devices = calloc(count, sizeof(*devices));
    if (!devices) goto out;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        printf("Vulkan name=%s vendor=0x%x device=0x%x type=%d api=%u.%u.%u driver=0x%x\n",
               p.deviceName, p.vendorID, p.deviceID, p.deviceType,
               VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
               VK_VERSION_PATCH(p.apiVersion), p.driverVersion);
        if (p.vendorID == 0x1002 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU &&
            !software(p.deviceName)) physical = devices[i];
    }
    if (!physical) goto out;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    families = calloc(count, sizeof(*families));
    if (!families) goto out;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            family = i; break;
        }
    if (family == UINT32_MAX) goto out;
    float priority = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VK_CHECK(vkCreateDevice(physical, &dci, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK_CHECK(vkCreateBuffer(device, &bci, NULL, &buffer));
    VkMemoryRequirements req;
    VkPhysicalDeviceMemoryProperties props;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags) { type = i; break; }
    if (type == UINT32_MAX) goto out;
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size, .memoryTypeIndex = type};
    VK_CHECK(vkAllocateMemory(device, &mai, NULL, &memory));
    VK_CHECK(vkBindBufferMemory(device, buffer, memory, 0));
    VK_CHECK(vkMapMemory(device, memory, 0, req.size, 0, &mapped));
    memset(mapped, 0, 4096);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .queueFamilyIndex = family};
    VK_CHECK(vkCreateCommandPool(device, &pci, NULL, &pool));
    VkCommandBuffer command;
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VK_CHECK(vkAllocateCommandBuffers(device, &cai, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(command, &begin));
    vkCmdFillBuffer(command, buffer, 0, 4096, 0x13579bdf);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    VK_CHECK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(device, &fci, NULL, &fence));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                          .commandBufferCount = 1, .pCommandBuffers = &command};
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    submitted = 1;
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ULL));
    completed = 1;
    for (size_t i = 0; i < 1024; ++i)
        if (((const uint32_t *)mapped)[i] != 0x13579bdf) goto out;
    printf("Vulkan graphics queue=%u: 4096-byte GPU fill/readback passed\n", family);
    ok = 1;
out:
    free(families);
    free(devices);
    /* A hung submission must not be destroyed while in flight. Exit terminates
     * the diagnostic process; the caller also applies a wall-clock timeout. */
    if (submitted && !completed) return 0;
    if (mapped) vkUnmapMemory(device, memory);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    if (buffer) vkDestroyBuffer(device, buffer, NULL);
    if (memory) vkFreeMemory(device, memory, NULL);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    return ok;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int egl_ok = egl_probe();
    int vk_ok = vk_probe();
    printf("RESULT EGL=%s VK=%s\n", egl_ok ? "PASS" : "FAIL", vk_ok ? "PASS" : "FAIL");
    return (egl_ok ? 0 : 1) | (vk_ok ? 0 : 2);
}
