param(
    [string]$VulkanInclude = $env:VULKAN_SDK
)
$ErrorActionPreference = 'Stop'

if (-not [Environment]::Is64BitProcess) {
    throw "Run this script from 64-bit PowerShell and an x64 Visual Studio Developer environment."
}
if (-not $VulkanInclude) {
    throw "VULKAN_SDK is not set. Pass -VulkanInclude <Vulkan SDK root> or set VULKAN_SDK."
}

$includeDir = Join-Path $VulkanInclude 'Include'
if (-not (Test-Path (Join-Path $includeDir 'vulkan\vulkan.h'))) {
    throw "Vulkan headers not found: $includeDir\vulkan\vulkan.h"
}
$systemLoader = Join-Path $env:WINDIR 'System32\vulkan-1.dll'
if (-not (Test-Path $systemLoader)) {
    throw "System Vulkan loader not found: $systemLoader. Install/update a Vulkan-capable display driver."
}

$exportsPath = Join-Path $PSScriptRoot 'vulkan-1-proxy.exports.txt'
if (-not (Test-Path $exportsPath)) {
    throw "Missing export list: $exportsPath"
}
$exports = @(
    Get-Content $exportsPath |
    ForEach-Object { $_.Trim() } |
    Where-Object { $_ -and -not $_.StartsWith(';') }
)

# These commands are implemented as C++ diagnostic wrappers below, not MASM thunks.

$localExports = @(
    'vkCreateDevice',
    'vkEnumerateDeviceExtensionProperties',
    'vkGetPhysicalDeviceProperties',
    'vkGetPhysicalDeviceProperties2',
    'vkGetPhysicalDeviceProperties2KHR',
    'vkGetPhysicalDeviceFeatures2',
    'vkGetPhysicalDeviceFeatures2KHR',
    'vkCreateGraphicsPipelines',
    'vkDestroyPipeline',
	'vkCreateComputePipelines',
    'vkCmdBindPipeline',
    'vkCmdBeginRendering',
    'vkCmdEndRendering',
    'vkCreateImageView',
    'vkDestroyImageView',
    'vkAllocateCommandBuffers',
    'vkFreeCommandBuffers',
    'vkGetDeviceBufferMemoryRequirements',
    'vkGetDeviceImageMemoryRequirements',
    'vkGetDeviceImageSparseMemoryRequirements',
    'vkQueueSubmit2',
    'vkQueueSubmit2KHR',
    'vkCmdPipelineBarrier2',
    'vkCmdPipelineBarrier2KHR',
    'vkCmdWriteTimestamp2',
    'vkCmdWriteTimestamp2KHR',
    'vkCreateSwapchainKHR',
    'vkCreateShaderModule',
	'vkDestroyShaderModule',
    'vkQueuePresentKHR',
    # v5: расширенная диагностика
    'vkCreateInstance',
    'vkCreateWin32SurfaceKHR',
    'vkGetPhysicalDeviceSurfaceCapabilitiesKHR',
    'vkGetPhysicalDeviceSurfaceFormatsKHR',
    'vkGetPhysicalDeviceSurfacePresentModesKHR',
    'vkCreateImage',
    'vkGetImageMemoryRequirements',
    'vkGetImageMemoryRequirements2',
    'vkAllocateMemory',
    'vkBindImageMemory',
    'vkBindImageMemory2',
    'vkCreateBuffer',
    'vkGetBufferMemoryRequirements',
    'vkGetBufferMemoryRequirements2',
    'vkBindBufferMemory',
    'vkBindBufferMemory2',
    'vkCreateDescriptorSetLayout',
    'vkCreatePipelineLayout',
	# v6: расширенная локализация краша
    'vkCreateSampler',
    'vkCreateDescriptorPool',
    'vkAllocateDescriptorSets',
    'vkUpdateDescriptorSets',
    'vkCreateCommandPool',
    'vkCreateFence',
    'vkCreateRenderPass',
	 'vkCmdDraw',
    'vkCmdDrawIndexed',
    'vkCmdDispatch',
    'vkCmdDrawIndirect',
	'vkCmdDispatchIndirect',
    'vkCmdDrawIndirectCount',
    'vkCmdDrawIndexedIndirectCount',
    'vkCmdDrawIndexedIndirect',
	 'vkCmdBindDescriptorSets',
    'vkCmdPushConstants',
    'vkCmdBindVertexBuffers',
    'vkCmdBindIndexBuffer',
	'vkCmdPipelineBarrier',
    'vkCreatePrivateDataSlot',
    'vkDestroyPrivateDataSlot',
    'vkSetPrivateData',
    'vkGetPrivateData',
    'vkCmdSetEvent2',
    'vkCmdResetEvent2',
    'vkCmdWaitEvents2',
    'vkCmdCopyBuffer2',
    'vkCmdCopyImage2',
    'vkCmdCopyBufferToImage2',
    'vkCmdCopyImageToBuffer2',
    'vkCmdBlitImage2',
    'vkCmdResolveImage2',
    'vkCmdBindVertexBuffers2'
)
$excludedExports = @('vkGetInstanceProcAddr', 'vkGetDeviceProcAddr') + $localExports
$exports = @($exports | Where-Object { $excludedExports -notcontains $_ })

# Safety check: local C++ wrappers must never also be emitted as MASM thunks.
$duplicateLocal = @($localExports | Where-Object { $exports -contains $_ })
if ($duplicateLocal.Count -gt 0) {
    throw "Internal export filtering error; local wrappers still in MASM list: $($duplicateLocal -join ', ')"
}

if ($exports.Count -eq 0) { throw 'The Vulkan export list is empty.' }

$seen = @{}
foreach ($name in $exports) {
    if ($name -notmatch '^vk[A-Za-z0-9_]+$') {
        throw "Invalid Vulkan export name: $name"
    }
    if ($seen.ContainsKey($name)) { throw "Duplicate Vulkan export name: $name" }
    $seen[$name] = $true
}

# Create the C++ name table used by the lazy resolver.
$headerLines = @(
    '#pragma once',
    "#define VK_PROXY_EXPORT_COUNT $($exports.Count)",
    'static constexpr const char* kVkProxyExportNames[VK_PROXY_EXPORT_COUNT] = {'
)
foreach ($name in $exports) { $headerLines += ('    "' + $name + '",') }
$headerLines += '};'
$headerPath = Join-Path $PSScriptRoot 'vk_proxy_exports.generated.h'
Set-Content -Path $headerPath -Value $headerLines -Encoding ASCII

# Generate true local code symbols. No .def forwarders are used: forwarding
# vulkan-1.dll to itself would recurse into this proxy.
$asmLines = @(
    'option casemap:none',
    'EXTERN g_vkSlots:QWORD',
    'EXTERN ResolveVkExport:PROC',
    '.code'
)
for ($i = 0; $i -lt $exports.Count; $i++) {
    $name = $exports[$i]
    $offset = $i * 8
    $asmLines += "PUBLIC $name"
    $asmLines += "$name PROC FRAME"
    $asmLines += '    sub rsp, 88h'
    $asmLines += '    .allocstack 88h'
    $asmLines += '    .endprolog'
    $asmLines += "    mov rax, QWORD PTR [g_vkSlots+$offset]"
    $asmLines += '    test rax, rax'
    $asmLines += "    jz ${name}_resolve"
    $asmLines += '    add rsp, 88h'
    $asmLines += '    jmp rax'
    $asmLines += "${name}_resolve:"
    $asmLines += '    mov QWORD PTR [rsp+20h], rcx'
    $asmLines += '    mov QWORD PTR [rsp+28h], rdx'
    $asmLines += '    mov QWORD PTR [rsp+30h], r8'
    $asmLines += '    mov QWORD PTR [rsp+38h], r9'
    $asmLines += '    movdqu XMMWORD PTR [rsp+40h], xmm0'
    $asmLines += '    movdqu XMMWORD PTR [rsp+50h], xmm1'
    $asmLines += '    movdqu XMMWORD PTR [rsp+60h], xmm2'
    $asmLines += '    movdqu XMMWORD PTR [rsp+70h], xmm3'
    $asmLines += "    mov ecx, $i"
    $asmLines += '    call ResolveVkExport'
    $asmLines += '    mov r11, rax'
    $asmLines += '    mov rcx, QWORD PTR [rsp+20h]'
    $asmLines += '    mov rdx, QWORD PTR [rsp+28h]'
    $asmLines += '    mov r8, QWORD PTR [rsp+30h]'
    $asmLines += '    mov r9, QWORD PTR [rsp+38h]'
    $asmLines += '    movdqu xmm0, XMMWORD PTR [rsp+40h]'
    $asmLines += '    movdqu xmm1, XMMWORD PTR [rsp+50h]'
    $asmLines += '    movdqu xmm2, XMMWORD PTR [rsp+60h]'
    $asmLines += '    movdqu xmm3, XMMWORD PTR [rsp+70h]'
    $asmLines += '    add rsp, 88h'
    $asmLines += '    test r11, r11'
    $asmLines += "    jz ${name}_missing"
    $asmLines += '    jmp r11'
    $asmLines += "${name}_missing:"
    $asmLines += '    xor eax, eax'
    $asmLines += '    ret'
    $asmLines += "$name ENDP"
}
$asmLines += 'END'
$asmPath = Join-Path $PSScriptRoot 'vk_proxy_thunks.generated.asm'
Set-Content -Path $asmPath -Value $asmLines -Encoding ASCII

# Export table binds to the actual MASM thunk symbols, not nonexistent C++ symbols.
$defLines = @(
    'LIBRARY "vulkan-1.dll"',
    'EXPORTS',
    '    vkGetInstanceProcAddr',
    '    vkGetDeviceProcAddr'
)
foreach ($name in $localExports) { $defLines += "    $name" }
foreach ($name in $exports) { $defLines += "    $name" }
$defPath = Join-Path $PSScriptRoot 'vulkan-1-proxy.def'
Set-Content -Path $defPath -Value $defLines -Encoding ASCII

$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
$ml64 = Get-Command ml64.exe -ErrorAction SilentlyContinue
$rc = Get-Command rc.exe -ErrorAction SilentlyContinue
if (-not $cl) { throw "cl.exe not found. Run from 'x64 Native Tools Command Prompt for VS 2026'." }
if (-not $ml64) { throw "ml64.exe not found. Install the MSVC x64/ARM64 build tools and run from the x64 Native Tools prompt." }
if (-not $rc) { throw "rc.exe not found. Ensure Windows SDK is installed and present in PATH." }

Push-Location $PSScriptRoot
try {
    $asmObj = Join-Path $PSScriptRoot 'vk_proxy_thunks.obj'
    $resObj = Join-Path $PSScriptRoot 'version.res'
    $rcPath = Join-Path $PSScriptRoot 'version.rc'
    $outDll = Join-Path $PSScriptRoot 'vulkan-1.dll'

    # Формируем структуру файла ресурсов без внутренних комментариев PowerShell
    $rcLines = @(
        '#include <winres.h>',
        '',
        'VS_VERSION_INFO VERSIONINFO',
        ' FILEVERSION 1,2,0,0',
        ' PRODUCTVERSION 1,2,0,0',
        ' FILEFLAGSMASK 0x3fL',
        '#ifdef _DEBUG',
        ' FILEFLAGS 0x1L',
        '#else',
        ' FILEFLAGS 0x0L',
        '#endif',
        ' FILEOS 0x40004L',
        ' FILETYPE 0x2L',
        ' FILESUBTYPE 0x0L',
        'BEGIN',
        '    BLOCK "StringFileInfo"',
        '    BEGIN',
        '        BLOCK "041904b0"',
        '        BEGIN',
        '            VALUE "CompanyName",      "ExEL@b"',
        '            VALUE "FileDescription",  "For Indiana.Jones GreatCircle"',
        '            VALUE "FileVersion",      "1.2.0.0"',
        '            VALUE "InternalName",     "vulkan-1"',
        '            VALUE "LegalCopyright",   "Copyright (C) 2026 Oparo"',
        '            VALUE "OriginalFilename", "vulkan-1.dll"',
        '            VALUE "ProductName",      "SpirVulkan RT Spoof"',
        '            VALUE "ProductVersion",   "1.2.0.0"',
        '        END',
        '    END',
        '    BLOCK "VarFileInfo"',
        '    BEGIN',
        '        VALUE "Translation", 0x419, 1200',
        '    END',
        'END'
    )
    
    # Сохраняем в UTF8 с BOM (через Out-File), чтобы rc.exe корректно прочитал кодировку
    $rcLines | Out-File -FilePath $rcPath -Encoding utf8

    # 1. Компиляция MASM thunks
    & $ml64.Source /nologo /c "/Fo$asmObj" $asmPath
    if ($LASTEXITCODE -ne 0) { throw "MASM failed with exit code $LASTEXITCODE" }

    # 2. Компиляция файла ресурсов .rc -> .res
    & $rc.Source /nologo "/fo$resObj" $rcPath
    if ($LASTEXITCODE -ne 0) { throw "Resource Compiler (rc.exe) failed with exit code $LASTEXITCODE" }

    # 3. Компиляция и линковка DLL (включая .obj и .res файлы)
    & $cl.Source /nologo /std:c++17 /EHsc /O2 /MT /LD /Zi "/I$includeDir" `
        dllmain.cpp $asmObj $resObj /link /DEBUG "/DEF:$defPath" /MACHINE:X64 "/OUT:$outDll"
    if ($LASTEXITCODE -ne 0) { throw "MSVC linker/compiler failed with exit code $LASTEXITCODE" }

    Write-Host "Built: $outDll" -ForegroundColor Green
    Write-Host "Exports: $($exports.Count + $localExports.Count + 2) (2 loader wrappers + $($localExports.Count) diagnostic wrappers + $($exports.Count) MASM thunks)" -ForegroundColor Green
    Write-Host 'v2: logs real GPU/apiVersion/extensions, spoofs apiVersion 1.3 + RT caps, sanitizes vkCreateDevice, stubs RT entry points.' -ForegroundColor Yellow
    Write-Host 'This proxy does NOT emulate Ray Tracing; it only tries to get the game past start-up checks.' -ForegroundColor Yellow
}
finally { Pop-Location }


