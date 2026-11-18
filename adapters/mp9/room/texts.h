#pragma once
// Adaptación mínima para compilar online.cpp/online.h SIN CAMBIOS, copiados de
// mp9-recomp/launcher/src (rama claude-markel/mp9, revisión 01a05ad):
//   online.cpp sha256 35f4fb8a438815a607033b843a2aa5d83f6e0a8584bf323504c307ee2642d240
//   online.h   sha256 907c7e4714ad85f7c43d46aa66392f54f6aa923876f8d637a1cae59bc7f1bb82
// El launcher original elige el idioma con language_is(); el unificado está en español.
inline bool language_is(const char* code) {
    return code && code[0] == 'e' && code[1] == 's';
}
