// Time RADV's compile of a SPIR-V module: vkCreateGraphicsPipelines with the module in one stage and a trivial shader
// in the other, no surface, no validation. usage: pipebench <stage: vs|fs> <a.spv> [a.spv ...]
// (the point is to tell a driver that takes 10 s per pipeline from our own code around it)
#include <shaderc/shaderc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint32_t *compile_glsl(const char *src, shaderc_shader_kind kind, size_t *n) {
    static shaderc_compiler_t c;
    if (!c) c = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(c, src, strlen(src), kind, "x", "main", o);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        fprintf(stderr, "glsl: %s\n", shaderc_result_get_error_message(r));
        exit(1);
    }
    size_t sz = shaderc_result_get_length(r);
    uint32_t *p = malloc(sz);
    memcpy(p, shaderc_result_get_bytes(r), sz);
    *n = sz;
    shaderc_result_release(r);
    shaderc_compile_options_release(o);
    return p;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s vs|fs a.spv ...\n", argv[0]);
        return 1;
    }
    int want_fs = !strcmp(argv[1], "fs");
    int want_cs = !strcmp(argv[1], "cs");
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { fprintf(stderr, "no instance\n"); return 1; }
    uint32_t nd = 0;
    vkEnumeratePhysicalDevices(inst, &nd, NULL);
    VkPhysicalDevice *pd = calloc(nd, sizeof *pd);
    vkEnumeratePhysicalDevices(inst, &nd, pd);
    if (!nd) { fprintf(stderr, "no device\n"); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd[0], &props);
    printf("device: %s\n", props.deviceName);
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    // every feature the device supports, enabled: our shaders want int64, subgroup ops and 8/16-bit storage
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    vkGetPhysicalDeviceFeatures2(pd[0], &f2);  // (what is supported; given back below = enable it all)
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                              .pNext = &f2};
    fprintf(stderr,"[b] creating device\n");
    VkDevice dev;
    if (vkCreateDevice(pd[0], &dci, NULL, &dev) != VK_SUCCESS) { fprintf(stderr, "no device2\n"); return 1; }

    fprintf(stderr,"[b] device ok\n");
    size_t tvn, tfn;
    uint32_t *tv = compile_glsl("#version 450\nvoid main(){ gl_Position = vec4(0.0); }\n", shaderc_vertex_shader, &tvn);
    uint32_t *tf = compile_glsl("#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(1.0); }\n",
                                shaderc_fragment_shader, &tfn);
    VkShaderModule mvs_triv, mfs_triv;
    VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = tvn, .pCode = tv};
    vkCreateShaderModule(dev, &smci, NULL, &mvs_triv);
    smci.codeSize = tfn, smci.pCode = tf;
    vkCreateShaderModule(dev, &smci, NULL, &mfs_triv);

    // one colour (RGBA8) + depth (D32), no MSAA: the shape of the passes the game's hot shaders run in
    VkAttachmentDescription at[2] = {
        {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference cr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dr = {1, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                               .pColorAttachments = &cr, .pDepthStencilAttachment = &dr};
    VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = at,
                                   .subpassCount = 1, .pSubpasses = &sp};
    fprintf(stderr,"[b] trivial modules ok\n");
    VkRenderPass rp;
    vkCreateRenderPass(dev, &rpci, NULL, &rp);

    // an empty layout: the shaders' descriptor sets are not needed to compile them
    // two sets with room for everything our shaders bind (set 0: cbufs/storage, set 1: samplers), plus push constants
    // (an empty layout makes RADV read a set it has no entry for while compiling: a segfault in vkCreate*Pipelines)
    VkDescriptorSetLayoutBinding b0[96], b1[96];
    for (int i = 0; i < 96; i++) {
        b0[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorCount = 1,
              .descriptorType = i < 64 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              .stageFlags = VK_SHADER_STAGE_ALL};
        b1[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .stageFlags = VK_SHADER_STAGE_ALL};
    }
    VkDescriptorSetLayout dsl[2];
    for (int k = 0; k < 2; k++) {
        VkDescriptorSetLayoutCreateInfo dci2 = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .bindingCount = 96, .pBindings = k ? b1 : b0};
        if (vkCreateDescriptorSetLayout(dev, &dci2, NULL, &dsl[k]) != VK_SUCCESS) { fprintf(stderr, "dsl failed\n"); return 1; }
    }
    VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_ALL, .offset = 0, .size = 256};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2,
                                       .pSetLayouts = dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    VkPipelineCacheCreateInfo pcci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache pc;
    vkCreatePipelineCache(dev, &pcci, NULL, &pc);

    fprintf(stderr,"[b] layout/cache ok\n");
    for (int i = 2; i < argc; i++) {
        FILE *fp = fopen(argv[i], "rb");
        if (!fp) { fprintf(stderr, "%s: cannot open\n", argv[i]); continue; }
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        uint32_t *code = malloc((size_t)n);
        if (fread(code, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(code); continue; }
        fclose(fp);
        VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = code};
        VkShaderModule m;
        if (vkCreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS) { printf("%s: module failed\n", argv[i]); free(code); continue; }
        fprintf(stderr,"[b] module ok\n");
        if (want_cs) {
            VkComputePipelineCreateInfo ci3 = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .flags = getenv("NOOPT") ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0,
                .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                          .module = m, .pName = "main"}, .layout = pl};
            VkPipeline pc2 = VK_NULL_HANDLE;
            double c0 = now_ms();
            VkResult rc = vkCreateComputePipelines(dev, pc, 1, &ci3, NULL, &pc2);
            double c1 = now_ms();
            printf("%9.1f ms  %s  %s\n", c1 - c0, rc == VK_SUCCESS ? "ok  " : "FAIL", argv[i]);
            fflush(stdout);
            if (pc2) vkDestroyPipeline(dev, pc2, NULL);
            vkDestroyShaderModule(dev, m, NULL);
            free(code);
            continue;
        }
        VkPipelineShaderStageCreateInfo st[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = want_fs ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT,
             .module = want_fs ? mvs_triv : m, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = want_fs ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT,
             .module = want_fs ? m : mfs_triv, .pName = "main"}};
        VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
                                               .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                    .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                  .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineDepthStencilStateCreateInfo dss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                    .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
                                                    .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
        VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
        VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                 .attachmentCount = 1, .pAttachments = &cba};
        VkGraphicsPipelineCreateInfo gci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                           .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
                                           .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &dss,
                                           .pColorBlendState = &cb, .layout = pl, .renderPass = rp};
        VkPipeline p = VK_NULL_HANDLE;
        fprintf(stderr,"[b] creating pipeline\n");
        double t0 = now_ms();
        VkResult r = vkCreateGraphicsPipelines(dev, pc, 1, &gci, NULL, &p);
        double t1 = now_ms();
        printf("%9.1f ms  %s  %s\n", t1 - t0, r == VK_SUCCESS ? "ok  " : "FAIL", argv[i]);
        fflush(stdout);
        if (p) vkDestroyPipeline(dev, p, NULL);
        vkDestroyShaderModule(dev, m, NULL);
        free(code);
    }
    return 0;
}
