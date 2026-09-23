#pragma once

// Derived from the 2026-09-23 CS2 items_game, shared response criteria,
// and Radio.* response groups intersected with shipped sound events.
// See doc/notes/cs2-agent-voice-catalog.md for identities and scope.
namespace MirvPovAgentVoice {
struct Agent { int definition; const char * family; };
static constexpr Agent kAgents[] = {
    {4613, "professional_epic"}, // tm_professional_varf5
    {4619, "seal"}, // ctm_st6_variantj
    {4680, "seal"}, // ctm_st6_variantl
    {4711, "swat_epic"}, // ctm_swat_variante
    {4712, "swat_fem"}, // ctm_swat_variantf
    {4713, "swat"}, // ctm_swat_variantg
    {4714, "swat"}, // ctm_swat_varianth
    {4715, "swat"}, // ctm_swat_varianti
    {4716, "swat"}, // ctm_swat_variantj
    {4718, "balkan"}, // tm_balkan_variantk
    {4726, "professional_epic"}, // tm_professional_varf
    {4727, "professional_fem"}, // tm_professional_varg
    {4728, "professional"}, // tm_professional_varh
    {4730, "professional_fem"}, // tm_professional_varj
    {4732, "professional"}, // tm_professional_vari
    {4733, "professional_epic"}, // tm_professional_varf1
    {4734, "professional_epic"}, // tm_professional_varf2
    {4735, "professional_epic"}, // tm_professional_varf3
    {4736, "professional_epic"}, // tm_professional_varf4
    {4749, "gendarmerie_male"}, // ctm_gendarmerie_varianta
    {4750, "gendarmerie_male"}, // ctm_gendarmerie_variantb
    {4751, "gendarmerie_fem_epic"}, // ctm_gendarmerie_variantc
    {4752, "gendarmerie_male"}, // ctm_gendarmerie_variantd
    {4753, "gendarmerie_male"}, // ctm_gendarmerie_variante
    {4756, "swat_fem"}, // ctm_swat_variantk
    {4757, "seal_fem"}, // ctm_diver_varianta
    {4771, "seal_diver_01"}, // ctm_diver_variantb
    {4772, "seal_diver_02"}, // ctm_diver_variantc
    {4773, "jungle_male"}, // tm_jungle_raider_varianta
    {4774, "jungle_male_epic"}, // tm_jungle_raider_variantb
    {4775, "jungle_male"}, // tm_jungle_raider_variantc
    {4776, "jungle_male"}, // tm_jungle_raider_variantd
    {4777, "jungle_fem_epic"}, // tm_jungle_raider_variante
    {4778, "jungle_fem"}, // tm_jungle_raider_variantf
    {4780, "jungle_male_epic"}, // tm_jungle_raider_variantb2
    {4781, "jungle_fem"}, // tm_jungle_raider_variantf2
    {5036, "phoenix"}, // t_map_based
    {5037, "sas"}, // ct_map_based
    {5038, "phoenix"}, // tm_anarchist
    {5039, "phoenix"}, // tm_anarchist_varianta
    {5040, "phoenix"}, // tm_anarchist_variantb
    {5041, "phoenix"}, // tm_anarchist_variantc
    {5042, "phoenix"}, // tm_anarchist_variantd
    {5043, "phoenix"}, // tm_pirate
    {5044, "phoenix"}, // tm_pirate_varianta
    {5045, "phoenix"}, // tm_pirate_variantb
    {5046, "phoenix"}, // tm_pirate_variantc
    {5047, "phoenix"}, // tm_pirate_variantd
    {5048, "phoenix"}, // tm_professional
    {5049, "phoenix"}, // tm_professional_var1
    {5050, "phoenix"}, // tm_professional_var2
    {5051, "phoenix"}, // tm_professional_var3
    {5052, "phoenix"}, // tm_professional_var4
    {5053, "phoenix"}, // tm_separatist
    {5054, "phoenix"}, // tm_separatist_varianta
    {5055, "phoenix"}, // tm_separatist_variantb
    {5056, "phoenix"}, // tm_separatist_variantc
    {5057, "phoenix"}, // tm_separatist_variantd
    {5058, "sas"}, // ctm_gign
    {5059, "sas"}, // ctm_gign_varianta
    {5060, "sas"}, // ctm_gign_variantb
    {5061, "sas"}, // ctm_gign_variantc
    {5062, "sas"}, // ctm_gign_variantd
    {5063, "sas"}, // ctm_gsg9
    {5064, "sas"}, // ctm_gsg9_varianta
    {5065, "sas"}, // ctm_gsg9_variantb
    {5066, "sas"}, // ctm_gsg9_variantc
    {5067, "sas"}, // ctm_gsg9_variantd
    {5068, "sas"}, // ctm_idf
    {5069, "sas"}, // ctm_idf_variantb
    {5070, "sas"}, // ctm_idf_variantc
    {5071, "sas"}, // ctm_idf_variantd
    {5072, "sas"}, // ctm_idf_variante
    {5073, "sas"}, // ctm_idf_variantf
    {5074, "sas"}, // ctm_swat
    {5075, "sas"}, // ctm_swat_varianta
    {5076, "sas"}, // ctm_swat_variantb
    {5077, "sas"}, // ctm_swat_variantc
    {5078, "sas"}, // ctm_swat_variantd
    {5079, "sas"}, // ctm_sas_varianta
    {5080, "sas"}, // ctm_sas_variantb
    {5081, "sas"}, // ctm_sas_variantc
    {5082, "sas"}, // ctm_sas_variantd
    {5083, "sas"}, // ctm_st6
    {5084, "sas"}, // ctm_st6_varianta
    {5085, "sas"}, // ctm_st6_variantb
    {5086, "sas"}, // ctm_st6_variantc
    {5087, "sas"}, // ctm_st6_variantd
    {5088, "phoenix"}, // tm_balkan_variante
    {5089, "phoenix"}, // tm_balkan_varianta
    {5090, "phoenix"}, // tm_balkan_variantb
    {5091, "phoenix"}, // tm_balkan_variantc
    {5092, "phoenix"}, // tm_balkan_variantd
    {5093, "phoenix"}, // tm_jumpsuit_varianta
    {5094, "phoenix"}, // tm_jumpsuit_variantb
    {5095, "phoenix"}, // tm_jumpsuit_variantc
    {5096, "phoenix"}, // tm_phoenix_heavy
    {5097, "sas"}, // ctm_heavy
    {5100, "leet"}, // tm_leet_varianta
    {5101, "leet"}, // tm_leet_variantb
    {5102, "leet"}, // tm_leet_variantc
    {5103, "leet"}, // tm_leet_variantd
    {5104, "leet"}, // tm_leet_variante
    {5105, "leet"}, // tm_leet_variantg
    {5106, "leet"}, // tm_leet_varianth
    {5107, "leet"}, // tm_leet_varianti
    {5108, "leet_epic"}, // tm_leet_variantf
    {5109, "leet"}, // tm_leet_variantj
    {5200, "phoenix"}, // tm_phoenix
    {5201, "phoenix"}, // tm_phoenix_varianta
    {5202, "phoenix"}, // tm_phoenix_variantb
    {5203, "phoenix"}, // tm_phoenix_variantc
    {5204, "phoenix"}, // tm_phoenix_variantd
    {5205, "phoenix"}, // tm_phoenix_varianth
    {5206, "phoenix"}, // tm_phoenix_variantf
    {5207, "phoenix"}, // tm_phoenix_variantg
    {5208, "phoenix"}, // tm_phoenix_varianti
    {5300, "fbihrt"}, // ctm_fbi
    {5301, "fbihrt"}, // ctm_fbi_varianta
    {5302, "fbihrt"}, // ctm_fbi_variantc
    {5303, "fbihrt"}, // ctm_fbi_variantd
    {5304, "fbihrt"}, // ctm_fbi_variante
    {5305, "fbihrt"}, // ctm_fbi_variantf
    {5306, "fbihrt"}, // ctm_fbi_variantg
    {5307, "fbihrt"}, // ctm_fbi_varianth
    {5308, "fbihrt_epic"}, // ctm_fbi_variantb
    {5400, "gsg9"}, // ctm_st6_variantk
    {5401, "seal"}, // ctm_st6_variante
    {5402, "seal"}, // ctm_st6_variantg
    {5403, "seal"}, // ctm_st6_variantm
    {5404, "seal_epic"}, // ctm_st6_varianti
    {5405, "seal"}, // ctm_st6_variantn
    {5500, "balkan"}, // tm_balkan_variantf
    {5501, "balkan"}, // tm_balkan_varianti
    {5502, "balkan"}, // tm_balkan_variantg
    {5503, "balkan"}, // tm_balkan_variantj
    {5504, "balkan_epic"}, // tm_balkan_varianth
    {5505, "balkan"}, // tm_balkan_variantl
    {5600, "sas"}, // ctm_sas
    {5601, "sas"}, // ctm_sas_variantf
    {5602, "sas"}, // ctm_sas_variantg
};

// Columns: smoke, flashbang, HE, molotov/incendiary, decoy.
struct Throws { const char * family; const char * stems[5][8]; };
static constexpr Throws kThrows[] = {
    {"balkan", {
        {"t_smoke01", "t_smoke02", "t_smoke03"},
        {"t_flashbang01", "t_flashbang02", "t_flashbang03"},
        {"t_grenade01", "t_grenade02", "t_grenade04"},
        {"t_molotov01", "t_molotov02", "t_molotov04"},
        {"t_decoy01", "t_decoy02", "t_decoy03"},
    }},
    {"balkan_epic", {
        {"throwing_smoke_01", "throwing_smoke_02", "throwing_smoke_03"},
        {"throwing_flashbang_01", "throwing_flashbang_02", "throwing_flashbang_03"},
        {"throwing_grenade_01", "throwing_grenade_02", "throwing_grenade_03"},
        {"throwing_molotov_01", "throwing_molotov_02", "throwing_molotov_03", "throwing_molotov_04", "throwing_molotov_05"},
        {"throwing_decoy_01", "throwing_decoy_02", "throwing_decoy_03", "throwing_decoy_04"},
    }},
    {"fbihrt", {
        {"ct_smoke01", "ct_smoke02", "ct_smoke04"},
        {"ct_flashbang01", "ct_flashbang02", "ct_flashbang03"},
        {"ct_grenade01", "ct_grenade02", "ct_grenade03"},
        {"ct_molotov02", "ct_molotov03", "ct_molotov04"},
        {"ct_decoy01", "ct_decoy02", "ct_decoy03"},
    }},
    {"fbihrt_epic", {
        {"throwing_smoke_01", "throwing_smoke_02", "throwing_smoke_03", "throwing_smoke_04", "throwing_smoke_05", "throwing_smoke_06"},
        {"throwing_flashbang_01", "throwing_flashbang_02", "throwing_flashbang_03", "throwing_flashbang_04", "throwing_flashbang_05", "throwing_flashbang_06"},
        {"throwing_grenade_01", "throwing_grenade_02", "throwing_grenade_03", "throwing_grenade_04", "throwing_grenade_05", "throwing_grenade_06", "throwing_grenade_07"},
        {"throwing_molotov_01", "throwing_molotov_02", "throwing_molotov_03", "throwing_molotov_04"},
        {"throwing_decoy_01", "throwing_decoy_02", "throwing_decoy_03"},
    }},
    {"gendarmerie_fem", {
        {"ff1_throwing_smoke_01", "ff1_throwing_smoke_02", "ff1_throwing_smoke_03"},
        {"ff1_throwing_flashbang_01", "ff1_throwing_flashbang_02", "ff1_throwing_flashbang_03"},
        {"ff1_throwing_grenade_01", "ff1_throwing_grenade_02", "ff1_throwing_grenade_03"},
        {"ff1_throwing_molotov_01", "ff1_throwing_molotov_02", "ff1_throwing_molotov_03"},
        {"ff1_throwing_decoy_01", "ff1_throwing_decoy_02", "ff1_throwing_decoy_03"},
    }},
    {"gendarmerie_fem_epic", {
        {"ff2_throwing_smoke_01", "ff2_throwing_smoke_02", "ff2_throwing_smoke_03"},
        {"ff2_throwing_flashbang_01", "ff2_throwing_flashbang_02", "ff2_throwing_flashbang_03"},
        {"ff2_throwing_grenade_01", "ff2_throwing_grenade_02", "ff2_throwing_grenade_03"},
        {"ff2_throwing_molotov_01", "ff2_throwing_molotov_02", "ff2_throwing_molotov_03"},
        {"ff2_throwing_decoy_01", "ff2_throwing_decoy_02", "ff2_throwing_decoy_03"},
    }},
    {"gendarmerie_male", {
        {"fm1_throwing_smoke_01", "fm1_throwing_smoke_02", "fm1_throwing_smoke_03"},
        {"fm1_throwing_flashbang_01", "fm1_throwing_flashbang_02", "fm1_throwing_flashbang_03"},
        {"fm1_throwing_grenade_01", "fm1_throwing_grenade_02", "fm1_throwing_grenade_03"},
        {"fm1_throwing_molotov_01", "fm1_throwing_molotov_02", "fm1_throwing_molotov_03"},
        {"fm1_throwing_decoy_01", "fm1_throwing_decoy_02", "fm1_throwing_decoy_03"},
    }},
    {"gsg9", {
        {"ct_smoke01", "ct_smoke02", "ct_smoke03"},
        {"ct_flashbang01", "ct_flashbang02", "ct_flashbang03"},
        {"ct_grenade01", "ct_grenade03", "ct_grenade02"},
        {"ct_molotov01", "ct_molotov02", "ct_molotov03"},
        {"ct_decoy01", "ct_decoy02", "ct_decoy03"},
    }},
    {"jungle_fem", {
        {"aff1_throwing_smoke_01", "aff1_throwing_smoke_02", "aff1_throwing_smoke_03"},
        {"aff1_throwing_flashbang_01", "aff1_throwing_flashbang_02", "aff1_throwing_flashbang_03"},
        {"aff1_throwing_grenade_01", "aff1_throwing_grenade_02", "aff1_throwing_grenade_03"},
        {"aff1_throwing_molotov_01", "aff1_throwing_molotov_02", "aff1_throwing_molotov_03"},
        {"aff1_throwing_decoy_01", "aff1_throwing_decoy_02", "aff1_throwing_decoy_03"},
    }},
    {"jungle_fem_epic", {
        {"aff1_throwing_smoke_01", "aff1_throwing_smoke_02", "aff1_throwing_smoke_03"},
        {"aff1_throwing_flashbang_01", "aff1_throwing_flashbang_02", "aff1_throwing_flashbang_03"},
        {"aff1_throwing_grenade_01", "aff1_throwing_grenade_02", "aff1_throwing_grenade_03"},
        {"aff1_throwing_molotov_01", "aff1_throwing_molotov_02", "aff1_throwing_molotov_03"},
        {"aff1_throwing_decoy_01", "aff1_throwing_decoy_02", "aff1_throwing_decoy_03"},
    }},
    {"jungle_male", {
        {"afm1_throwing_smoke_01", "afm1_throwing_smoke_02", "afm1_throwing_smoke_03"},
        {"afm1_throwing_flashbang_01", "afm1_throwing_flashbang_02", "afm1_throwing_flashbang_03"},
        {"afm1_throwing_grenade_01", "afm1_throwing_grenade_02", "afm1_throwing_grenade_03"},
        {"afm1_throwing_molotov_01", "afm1_throwing_molotov_02", "afm1_throwing_molotov_03"},
        {"afm1_throwing_decoy_01", "afm1_throwing_decoy_02", "afm1_throwing_decoy_03"},
    }},
    {"jungle_male_epic", {
        {"afm2_throwing_smoke_01", "afm2_throwing_smoke_02", "afm2_throwing_smoke_03"},
        {"afm2_throwing_flashbang_01", "afm2_throwing_flashbang_02", "afm2_throwing_flashbang_03"},
        {"afm2_throwing_grenade_01", "afm2_throwing_grenade_02", "afm2_throwing_grenade_03"},
        {"afm2_throwing_molotov_01", "afm2_throwing_molotov_02", "afm2_throwing_molotov_03"},
        {"afm2_throwing_decoy_01", "afm2_throwing_decoy_02", "afm2_throwing_decoy_03"},
    }},
    {"leet", {
        {"t_smoke01", "t_smoke02", "t_smoke03"},
        {"t_flashbang01", "t_flashbang03", "t_flashbang04"},
        {"t_grenade01", "t_grenade02", "t_grenade03", "t_grenade05"},
        {"t_molotov01", "t_molotov02", "t_molotov04", "t_molotov05"},
        {"t_decoy01", "t_decoy02", "t_decoy03"},
    }},
    {"leet_epic", {
        {"throwing_smoke_01", "throwing_smoke_02", "throwing_smoke_03", "throwing_smoke_04", "throwing_smoke_05", "throwing_smoke_06"},
        {"throwing_flashbang_01", "throwing_flashbang_02", "throwing_flashbang_03"},
        {"throwing_grenade_01", "throwing_grenade_02", "throwing_grenade_03"},
        {"throwing_molotov_01", "throwing_molotov_02", "throwing_molotov_03", "throwing_molotov_04"},
        {"throwing_decoy_01", "throwing_decoy_02", "throwing_decoy_03"},
    }},
    {"phoenix", {
        {"t_smoke01", "t_smoke02", "t_smoke03"},
        {"t_flashbang01", "t_flashbang02", "t_flashbang03"},
        {"t_grenade02", "t_grenade04", "t_grenade05"},
        {"t_molotov01", "t_molotov02", "t_molotov03", "t_molotov04", "t_molotov05", "t_molotov07"},
        {"t_decoy01", "t_decoy02", "t_decoy03"},
    }},
    {"professional", {
        {"t_smoke01", "t_smoke03", "t_smoke04", "t_smoke05"},
        {"t_flashbang01", "t_flashbang03", "t_flashbang04"},
        {"t_grenade01", "t_grenade02", "t_grenade05", "t_grenade06"},
        {"t_molotov01", "t_molotov03", "t_molotov04", "t_molotov08", "t_molotov11"},
        {"t_decoy01", "t_decoy02", "t_decoy03"},
    }},
    {"professional_epic", {
        {"throwing_smoke_01", "throwing_smoke_02", "throwing_smoke_03"},
        {"throwing_flashbang_01", "throwing_flashbang_02", "throwing_flashbang_03"},
        {"throwing_grenade_01", "throwing_grenade_02", "throwing_grenade_03"},
        {"throwing_molotov_01", "throwing_molotov_02", "throwing_molotov_03", "throwing_molotov_04"},
        {"throwing_decoy_01", "throwing_decoy_02", "throwing_decoy_03"},
    }},
    {"professional_fem", {
        {"throwing_smoke_01", "throwing_smoke_02", "throwing_smoke_03", "throwing_smoke_04"},
        {"throwing_flashbang_01", "throwing_flashbang_02", "throwing_flashbang_03", "throwing_flashbang_04"},
        {"throwing_grenade_02", "throwing_grenade_03"},
        {"throwing_molotov_01", "throwing_molotov_02", "throwing_molotov_03", "throwing_molotov_04"},
        {"throwing_decoy_02", "throwing_decoy_03"},
    }},
    {"sas", {
        {"ct_smoke01", "ct_smoke02", "ct_smoke03"},
        {"ct_flashbang01", "ct_flashbang02", "ct_flashbang03"},
        {"ct_grenade01", "ct_grenade03", "ct_grenade04", "ct_grenade05"},
        {"ct_molotov01", "ct_molotov02", "ct_molotov03", "ct_molotov04"},
        {"ct_decoy01", "ct_decoy02", "ct_decoy03"},
    }},
    {"seal", {
        {"ct_smoke01", "ct_smoke02", "ct_smoke03"},
        {"ct_flashbang01", "ct_flashbang02", "ct_flashbang03"},
        {"ct_grenade01", "ct_grenade02", "ct_grenade03", "ct_grenade04"},
        {"ct_molotov01", "ct_molotov02", "ct_molotov03"},
        {"ct_decoy01", "ct_decoy02", "ct_decoy03"},
    }},
    {"seal_diver_01", {
        {"am1_throwing_smoke_01", "am1_throwing_smoke_02", "am1_throwing_smoke_03"},
        {"am1_throwing_flashbang_01", "am1_throwing_flashbang_02", "am1_throwing_flashbang_03"},
        {"am1_throwing_grenade_01", "am1_throwing_grenade_02", "am1_throwing_grenade_03"},
        {"am1_throwing_molotov_01", "am1_throwing_molotov_02", "am1_throwing_molotov_03"},
        {"am1_throwing_decoy_01", "am1_throwing_decoy_02", "am1_throwing_decoy_03"},
    }},
    {"seal_diver_02", {
        {"am1_throwing_smoke_01", "am1_throwing_smoke_02", "am1_throwing_smoke_03"},
        {"am1_throwing_flashbang_01", "am1_throwing_flashbang_02", "am1_throwing_flashbang_03"},
        {"am1_throwing_grenade_01", "am1_throwing_grenade_02", "am1_throwing_grenade_03"},
        {"am1_throwing_molotov_01", "am1_throwing_molotov_02", "am1_throwing_molotov_03"},
        {"am1_throwing_decoy_01", "am1_throwing_decoy_02", "am1_throwing_decoy_03"},
    }},
    {"seal_diver_03", {
        {"am3_throwing_smoke_01", "am3_throwing_smoke_02", "am3_throwing_smoke_03"},
        {"am3_throwing_flashbang_01", "am3_throwing_flashbang_02", "am3_throwing_flashbang_03"},
        {"am3_throwing_grenade_01", "am3_throwing_grenade_02", "am3_throwing_grenade_03"},
        {"am3_throwing_molotov_01", "am3_throwing_molotov_02", "am3_throwing_molotov_03"},
        {"am3_throwing_decoy_01", "am3_throwing_decoy_02", "am3_throwing_decoy_03"},
    }},
    {"seal_epic", {
        {"throwing_smoke_01", "throwing_smoke_02", "throwing_smoke_03", "throwing_smoke_04", "throwing_smoke_05", "throwing_smoke_06"},
        {"throwing_flashbang_01", "throwing_flashbang_02", "throwing_flashbang_03"},
        {"throwing_grenade_01", "throwing_grenade_02", "throwing_grenade_03"},
        {"throwing_molotov_01", "throwing_molotov_02", "throwing_molotov_03"},
        {"throwing_decoy_01", "throwing_decoy_02", "throwing_decoy_03"},
    }},
    {"seal_fem", {
        {"af1_throwing_smoke_01", "af1_throwing_smoke_02", "af1_throwing_smoke_03"},
        {"af1_throwing_flashbang_03"},
        {"af1_throwing_grenade_01", "af1_throwing_grenade_02", "af1_throwing_grenade_03"},
        {"af1_throwing_molotov_01", "af1_throwing_molotov_02", "af1_throwing_molotov_03"},
        {"af1_throwing_decoy_01", "af1_throwing_decoy_02", "af1_throwing_decoy_03"},
    }},
    {"swat", {
        {"ct_smoke01", "ct_smoke02", "ct_smoke03"},
        {"ct_flashbang01", "ct_flashbang02", "ct_flashbang03"},
        {"ct_grenade01", "ct_grenade04", "ct_grenade05", "ct_grenade06"},
        {"ct_molotov01", "ct_molotov02", "ct_molotov03", "ct_molotov04"},
        {"ct_decoy01", "ct_decoy02", "ct_decoy03"},
    }},
    {"swat_epic", {
        {"throwing_smoke_01", "throwing_smoke_02", "throwing_smoke_03", "throwing_smoke_04"},
        {"throwing_flashbang_01", "throwing_flashbang_02", "throwing_flashbang_03", "throwing_flashbang_04"},
        {"throwing_grenade_01", "throwing_grenade_02", "throwing_grenade_03", "throwing_grenade_04"},
        {"throwing_molotov_01", "throwing_molotov_02", "throwing_molotov_03", "throwing_molotov_04"},
        {"throwing_decoy_01", "throwing_decoy_02", "throwing_decoy_03"},
    }},
    {"swat_fem", {
        {"throwing_smoke_01", "throwing_smoke_02", "throwing_smoke_03"},
        {"throwing_flashbang_01", "throwing_flashbang_03"},
        {"throwing_grenade_02", "throwing_grenade_03"},
        {"throwing_molotov_01", "throwing_molotov_02"},
        {"throwing_decoy_01", "throwing_decoy_02", "throwing_decoy_03"},
    }},
};
} // namespace MirvPovAgentVoice
