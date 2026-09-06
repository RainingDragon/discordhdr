; DiscordHDRFix v1.0.1 automatic HDR/SDR relay
;
; The patched Video Hook CALL enters here with Discord's original register and
; stack state intact:
;
;   r12 = WumpusFrame*
;   r9  = renderer source descriptor (stack-local in Video Hook)
;   [rsp+48h] = ninth renderer argument after CALL pushed the return address
;
; The exact analyzed build stores:
;   WumpusFrame::is_source_hdr at [r12+1dah]
;
; Automatic policy:
;   is_source_hdr == 0
;       -> untouched SDR path, no metadata injection
;
;   is_source_hdr != 0 && DXGI_FORMAT == 24
;       -> Rec.2020 + ST.2084/PQ + HDR metadata
;
;   is_source_hdr != 0 && DXGI_FORMAT == 10
;       -> Rec.709 + Linear/scRGB + HDR metadata
;
;   is_source_hdr != 0 && other format
;       -> preserve color enum + inject HDR metadata
;
; The function never calls another helper and never changes RSP. It tail-jumps
; into Discord's original renderer wrapper, whose RET returns to the original
; Video Hook callsite.

OPTION CASEMAP:NONE

EXTERN g_DH_RuntimeEnabled:DWORD
EXTERN g_DH_DetectionMode:DWORD
EXTERN g_DH_ActiveMetadataPtr:QWORD
EXTERN g_DH_OriginalRenderer:QWORD

EXTERN g_DH_RendererCalls:QWORD
EXTERN g_DH_DisabledBypassFrames:QWORD
EXTERN g_DH_SdrFramesBypassed:QWORD
EXTERN g_DH_HdrFramesCorrected:QWORD
EXTERN g_DH_Hdr10Frames:QWORD
EXTERN g_DH_ScRgbFrames:QWORD
EXTERN g_DH_HdrUnknownFormatFrames:QWORD
EXTERN g_DH_MetadataInjected:QWORD
EXTERN g_DH_SourceColorOverrides:QWORD

EXTERN g_DH_LastSourceIsHdr:DWORD
EXTERN g_DH_LastSourceFormat:DWORD
EXTERN g_DH_LastOriginalPrimaries:DWORD
EXTERN g_DH_LastOriginalTransfer:DWORD
EXTERN g_DH_LastEffectivePrimaries:DWORD
EXTERN g_DH_LastEffectiveTransfer:DWORD
EXTERN g_DH_LastDecision:DWORD
EXTERN g_DH_LastOriginalHdrMetadataNull:DWORD

PUBLIC DiscordHDRFixRelay

.code

DiscordHDRFixRelay PROC

    lock inc qword ptr [g_DH_RendererCalls]

    ; Record whether Discord originally supplied arg9 metadata.
    xor eax, eax
    cmp qword ptr [rsp+48h], 0
    sete al
    mov dword ptr [g_DH_LastOriginalHdrMetadataNull], eax

    ; Record the real WumpusFrame HDR classification.
    movzx eax, byte ptr [r12+1dah]
    mov dword ptr [g_DH_LastSourceIsHdr], eax

    ; Record source descriptor before any correction.
    mov r10d, dword ptr [r9+178h]
    mov dword ptr [g_DH_LastSourceFormat], r10d

    movzx r11d, byte ptr [r9+17ch]
    mov dword ptr [g_DH_LastOriginalPrimaries], r11d
    mov dword ptr [g_DH_LastEffectivePrimaries], r11d

    movzx r11d, byte ptr [r9+17dh]
    mov dword ptr [g_DH_LastOriginalTransfer], r11d
    mov dword ptr [g_DH_LastEffectiveTransfer], r11d

    ; Global correction off = leave Discord's original SDR/Video Hook call
    ; completely untouched.
    cmp dword ptr [g_DH_RuntimeEnabled], 0
    je DH_Disabled

    ; Manual modes are escape hatches. Automatic is 0.
    mov eax, dword ptr [g_DH_DetectionMode]
    cmp eax, 1
    je DH_Sdr
    cmp eax, 2
    je DH_Hdr10
    cmp eax, 3
    je DH_ScRgb

    ; Automatic: this is the key v1.0.1 change.
    cmp byte ptr [r12+1dah], 0
    je DH_Sdr

    mov eax, dword ptr [r9+178h]
    cmp eax, 24
    je DH_Hdr10
    cmp eax, 10
    je DH_ScRgb
    jmp DH_HdrUnknown

DH_Disabled:
    lock inc qword ptr [g_DH_DisabledBypassFrames]
    mov dword ptr [g_DH_LastDecision], 0
    jmp DH_Tail

DH_Sdr:
    ; Critical SDR behavior: no fake HDR metadata and no color-space override.
    lock inc qword ptr [g_DH_SdrFramesBypassed]
    mov dword ptr [g_DH_LastDecision], 1
    jmp DH_Tail

DH_Hdr10:
    lock inc qword ptr [g_DH_HdrFramesCorrected]
    lock inc qword ptr [g_DH_Hdr10Frames]
    mov dword ptr [g_DH_LastDecision], 2

    ; R10G10B10A2 HDR frames: Rec.2020 + ST.2084/PQ.
    mov byte ptr [r9+17ch], 1
    mov byte ptr [r9+17dh], 2
    mov dword ptr [g_DH_LastEffectivePrimaries], 1
    mov dword ptr [g_DH_LastEffectiveTransfer], 2
    lock inc qword ptr [g_DH_SourceColorOverrides]
    jmp DH_InjectMetadata

DH_ScRgb:
    lock inc qword ptr [g_DH_HdrFramesCorrected]
    lock inc qword ptr [g_DH_ScRgbFrames]
    mov dword ptr [g_DH_LastDecision], 3

    ; FP16 scRGB HDR frames: Rec.709 primaries + linear transfer.
    mov byte ptr [r9+17ch], 0
    mov byte ptr [r9+17dh], 0
    mov dword ptr [g_DH_LastEffectivePrimaries], 0
    mov dword ptr [g_DH_LastEffectiveTransfer], 0
    lock inc qword ptr [g_DH_SourceColorOverrides]
    jmp DH_InjectMetadata

DH_HdrUnknown:
    ; We trust Discord's actual HDR bit but avoid guessing the encoding for an
    ; unknown texture format. Supplying the missing HDR metadata is still useful.
    lock inc qword ptr [g_DH_HdrFramesCorrected]
    lock inc qword ptr [g_DH_HdrUnknownFormatFrames]
    mov dword ptr [g_DH_LastDecision], 4

DH_InjectMetadata:
    mov rax, qword ptr [g_DH_ActiveMetadataPtr]
    test rax, rax
    je DH_Tail

    mov qword ptr [rsp+48h], rax
    lock inc qword ptr [g_DH_MetadataInjected]

DH_Tail:
    mov rax, qword ptr [g_DH_OriginalRenderer]
    jmp rax

DiscordHDRFixRelay ENDP

END
