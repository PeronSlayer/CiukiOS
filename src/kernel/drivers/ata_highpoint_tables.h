/* Linux v6.12 HighPoint timing tables and drive errata. Original notices
 * are retained in docs/licenses/ata-addin-NOTICES.txt.
 * SPDX-License-Identifier: GPL-2.0-only */
static const uint32_t hpt36_udma[][7] = {
    {0x90cb8521, 0x90cb8521, 0x90cf8521, 0x90cf8521, 0x90c98521, 0, 0},
    {0x90c8a731, 0x90cba731, 0x90caa731, 0x90cfa731, 0x90c9a731, 0, 0},
    {0x9008d943, 0x9008d943, 0x900bd943, 0x900ad943, 0x900fd943, 0, 0}
};
static const uint32_t hpt36_pio0[] = {0xc0d08585, 0xc0d0a7aa, 0xc018d9d9};
static const uint32_t hpt37_udma[][7] = {
    {0x12506297, 0x124c6233, 0x12486231, 0x126c6231, 0x12446231, 0x12446231, 0x12446231},
    {0x121882ea, 0x12148254, 0x120c8242, 0x128c8242, 0x12ac8242, 0x12848242, 0x12848242},
    {0x1c829c62, 0x1c9a9c62, 0x1c929c62, 0x1c8e9c62, 0x1c8a9c62, 0x1cae9c62, 0x1c869c62}
};
static const uint32_t hpt37_pio0[] = {0x06814ea7, 0x0ac1f48a, 0x0d029d5e};
static const uint32_t hptn_udma[][7] = {
    {0x1c829c62, 0x1c9a9c62, 0x1c929c62, 0x1c8e9c62, 0x1c8a9c62, 0x1c8a9c62, 0x1c869c62}
};
static const uint32_t hptn_pio0[] = {0x0d029d5e};
static const char *const hpt_bad33[] = {
	"Maxtor 92720U8", "Maxtor 92040U6", "Maxtor 91360U4", "Maxtor 91020U3",
	"Maxtor 90845U3", "Maxtor 90650U2",
	"Maxtor 91360D8", "Maxtor 91190D7", "Maxtor 91020D6", "Maxtor 90845D5",
	"Maxtor 90680D4", "Maxtor 90510D3", "Maxtor 90340D2",
	"Maxtor 91152D8", "Maxtor 91008D7", "Maxtor 90845D6", "Maxtor 90840D6",
	"Maxtor 90720D5", "Maxtor 90648D5", "Maxtor 90576D4",
	"Maxtor 90510D4",
	"Maxtor 90432D3", "Maxtor 90288D2", "Maxtor 90256D2",
	"Maxtor 91000D8", "Maxtor 90910D8", "Maxtor 90875D7", "Maxtor 90840D7",
	"Maxtor 90750D6", "Maxtor 90625D5", "Maxtor 90500D4",
	"Maxtor 91728D8", "Maxtor 91512D7", "Maxtor 91303D6", "Maxtor 91080D5",
	"Maxtor 90845D4", "Maxtor 90680D4", "Maxtor 90648D3", "Maxtor 90432D2",
	0
};
static const char *const hpt_bad66[] = {
	"IBM-DTLA-307075",
	"IBM-DTLA-307060",
	"IBM-DTLA-307045",
	"IBM-DTLA-307030",
	"IBM-DTLA-307020",
	"IBM-DTLA-307015",
	"IBM-DTLA-305040",
	"IBM-DTLA-305030",
	"IBM-DTLA-305020",
	"IC35L010AVER07-0",
	"IC35L020AVER07-0",
	"IC35L030AVER07-0",
	"IC35L040AVER07-0",
	"IC35L060AVER07-0",
	"WDC AC310200R",
	0
};
static const char *const hpt_bad66_3[] = {
	"WDC AC310200R",
	0
};
