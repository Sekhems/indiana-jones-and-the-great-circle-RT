// ---------------------------------------------------------------------------
// spv_check.exe - standalone portability check for the spv_replace folder.
//
// GENERATED FILE - do not edit. Change dllmain.cpp and run:
//     python tools/make_spv_check.py
//
// It uses the SAME matching code as the proxy, so what it reports is exactly
// what the game will see on a machine it has never run on.
//
// Build:  build_spv_check.bat   (needs MSVC only, no Vulkan SDK)
// Usage:  spv_check.exe ["D:\path\to\spv_replace"]
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

// Nothing is stubbed here on purpose: sprintf_s, _snprintf_s, SRWLOCK,
// SRWLOCK_INIT and the Interlocked* helpers all come from the Windows headers,
// exactly as they do in the proxy itself. Redefining them breaks the build
// (C2556/C2371/C2491/C2169).

// Diagnostics: same role as in the DLL, but written to the console.
static void LogLine(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

struct SpvOpDesc { uint16_t op; const char* kinds; };

namespace spvrw {
static const SpvOpDesc kSpvOps[] = {
    {0,""},
    {1,"TR"},
    {2,"S"},
    {3,"LLIS"},
    {4,"S"},
    {5,"IS"},
    {6,"ILS"},
    {7,"RS"},
    {8,"ILL"},
    {10,"S"},
    {11,"RS"},
    {12,"TRIL(I)"},
    {14,"LL"},
    {15,"LIS(I)"},
    {16,"IL"},
    {17,"L"},
    {19,"R"},
    {20,"R"},
    {21,"RLL"},
    {22,"RLL"},
    {23,"RIL"},
    {24,"RIL"},
    {25,"RILLLLLLL"},
    {26,"R"},
    {27,"RI"},
    {28,"RII"},
    {29,"RI"},
    {30,"R(I)"},
    {31,"RS"},
    {32,"RLI"},
    {33,"RI(I)"},
    {34,"R"},
    {35,"R"},
    {36,"R"},
    {37,"R"},
    {38,"RL"},
    {39,"IL"},
    {41,"TR"},
    {42,"TR"},
    {43,"TRN"},
    {44,"TR(I)"},
    {45,"TRLLL"},
    {46,"TR"},
    {48,"TR"},
    {49,"TR"},
    {50,"TRN"},
    {51,"TR(I)"},
    {52,"TRL"},
    {54,"TRLI"},
    {55,"TR"},
    {56,""},
    {57,"TRI(I)"},
    {59,"TRLI"},
    {60,"TRIII"},
    {61,"TRIA"},
    {62,"IIA"},
    {63,"IIAA"},
    {64,"IIIAA"},
    {65,"TRI(I)"},
    {66,"TRI(I)"},
    {67,"TRII(I)"},
    {68,"TRIL"},
    {69,"TRI"},
    {70,"TRII(I)"},
    {71,"IL"},
    {72,"ILL"},
    {73,"R"},
    {74,"I(I)"},
    {75,"I(IL)"},
    {77,"TRII"},
    {78,"TRIII"},
    {79,"TRII(L)"},
    {80,"TR(I)"},
    {81,"TRI(L)"},
    {82,"TRII(L)"},
    {83,"TRI"},
    {84,"TRI"},
    {86,"TRII"},
    {87,"TRIIM"},
    {88,"TRIIM"},
    {89,"TRIIIM"},
    {90,"TRIIIM"},
    {91,"TRIIM"},
    {92,"TRIIM"},
    {93,"TRIIIM"},
    {94,"TRIIIM"},
    {95,"TRIIM"},
    {96,"TRIIIM"},
    {97,"TRIIIM"},
    {98,"TRIIM"},
    {99,"IIIM"},
    {100,"TRI"},
    {101,"TRI"},
    {102,"TRI"},
    {103,"TRII"},
    {104,"TRI"},
    {105,"TRII"},
    {106,"TRI"},
    {107,"TRI"},
    {109,"TRI"},
    {110,"TRI"},
    {111,"TRI"},
    {112,"TRI"},
    {113,"TRI"},
    {114,"TRI"},
    {115,"TRI"},
    {116,"TRI"},
    {117,"TRI"},
    {118,"TRI"},
    {119,"TRI"},
    {120,"TRI"},
    {121,"TRI"},
    {122,"TRI"},
    {123,"TRIL"},
    {124,"TRI"},
    {126,"TRI"},
    {127,"TRI"},
    {128,"TRII"},
    {129,"TRII"},
    {130,"TRII"},
    {131,"TRII"},
    {132,"TRII"},
    {133,"TRII"},
    {134,"TRII"},
    {135,"TRII"},
    {136,"TRII"},
    {137,"TRII"},
    {138,"TRII"},
    {139,"TRII"},
    {140,"TRII"},
    {141,"TRII"},
    {142,"TRII"},
    {143,"TRII"},
    {144,"TRII"},
    {145,"TRII"},
    {146,"TRII"},
    {147,"TRII"},
    {148,"TRII"},
    {149,"TRII"},
    {150,"TRII"},
    {151,"TRII"},
    {152,"TRII"},
    {154,"TRI"},
    {155,"TRI"},
    {156,"TRI"},
    {157,"TRI"},
    {158,"TRI"},
    {159,"TRI"},
    {160,"TRI"},
    {161,"TRII"},
    {162,"TRII"},
    {163,"TRII"},
    {164,"TRII"},
    {165,"TRII"},
    {166,"TRII"},
    {167,"TRII"},
    {168,"TRI"},
    {169,"TRIII"},
    {170,"TRII"},
    {171,"TRII"},
    {172,"TRII"},
    {173,"TRII"},
    {174,"TRII"},
    {175,"TRII"},
    {176,"TRII"},
    {177,"TRII"},
    {178,"TRII"},
    {179,"TRII"},
    {180,"TRII"},
    {181,"TRII"},
    {182,"TRII"},
    {183,"TRII"},
    {184,"TRII"},
    {185,"TRII"},
    {186,"TRII"},
    {187,"TRII"},
    {188,"TRII"},
    {189,"TRII"},
    {190,"TRII"},
    {191,"TRII"},
    {194,"TRII"},
    {195,"TRII"},
    {196,"TRII"},
    {197,"TRII"},
    {198,"TRII"},
    {199,"TRII"},
    {200,"TRI"},
    {201,"TRIIII"},
    {202,"TRIII"},
    {203,"TRIII"},
    {204,"TRI"},
    {205,"TRI"},
    {207,"TRI"},
    {208,"TRI"},
    {209,"TRI"},
    {210,"TRI"},
    {211,"TRI"},
    {212,"TRI"},
    {213,"TRI"},
    {214,"TRI"},
    {215,"TRI"},
    {218,""},
    {219,""},
    {220,"I"},
    {221,"I"},
    {224,"III"},
    {225,"II"},
    {227,"TRIII"},
    {228,"IIII"},
    {229,"TRIIII"},
    {230,"TRIIIIII"},
    {231,"TRIIIIII"},
    {232,"TRIII"},
    {233,"TRIII"},
    {234,"TRIIII"},
    {235,"TRIIII"},
    {236,"TRIIII"},
    {237,"TRIIII"},
    {238,"TRIIII"},
    {239,"TRIIII"},
    {240,"TRIIII"},
    {241,"TRIIII"},
    {242,"TRIIII"},
    {245,"TR(II)"},
    {246,"IIL"},
    {247,"IL"},
    {248,"R"},
    {249,"I"},
    {250,"III(L)"},
    {251,"II(LI)"},
    {252,""},
    {253,""},
    {254,"I"},
    {255,""},
    {256,"IL"},
    {257,"IL"},
    {259,"TRIIIIII"},
    {260,"III"},
    {261,"TRII"},
    {262,"TRII"},
    {263,"TRIII"},
    {264,"TRILI"},
    {265,"TRILI"},
    {266,"TRILI"},
    {267,"TRILI"},
    {268,"TRILI"},
    {269,"TRILI"},
    {270,"TRILI"},
    {271,"TRILI"},
    {274,"TRIIII"},
    {275,"TRIIII"},
    {276,"TRIIIIII"},
    {277,"TRIIIIII"},
    {278,"TRIIII"},
    {279,"TRIIII"},
    {280,"IIII"},
    {281,"IIII"},
    {282,"TRI"},
    {283,"TRIII"},
    {284,"TRIII"},
    {285,"TRIIIII"},
    {286,"TRIIIII"},
    {287,"IIIII"},
    {288,"IIIII"},
    {291,"TRIIII"},
    {292,"TRIIIIIIIIII(I)"},
    {293,"TRIIIII"},
    {294,"TRIIIII"},
    {295,"TRIIII"},
    {296,"TRIIII"},
    {297,"I"},
    {298,"I"},
    {299,"TR"},
    {300,"TRI"},
    {301,"II"},
    {302,"III"},
    {303,"TR"},
    {304,"TRIII"},
    {305,"TRIIM"},
    {306,"TRIIM"},
    {307,"TRIIIM"},
    {308,"TRIIIM"},
    {309,"TRIIM"},
    {310,"TRIIM"},
    {311,"TRIIIM"},
    {312,"TRIIIM"},
    {313,"TRIIM"},
    {314,"TRIIIM"},
    {315,"TRIIIM"},
    {316,"TRI"},
    {317,""},
    {318,"TRIII"},
    {319,"III"},
    {320,"TRIIM"},
    {321,"TRI"},
    {322,"R"},
    {323,"TRLLL"},
    {324,"TRI"},
    {325,"TRIIIII"},
    {326,"TRIIII"},
    {327,"R"},
    {328,"TRI"},
    {329,"III"},
    {330,"S"},
    {331,"IL"},
    {332,"IL"},
    {333,"TRI"},
    {334,"TRII"},
    {335,"TRII"},
    {336,"TRII"},
    {337,"TRIII"},
    {338,"TRII"},
    {339,"TRII"},
    {340,"TRII"},
    {341,"TRIII"},
    {342,"TRILI"},
    {343,"TRII"},
    {344,"TRII"},
    {345,"TRIII"},
    {346,"TRIII"},
    {347,"TRIII"},
    {348,"TRIII"},
    {349,"TRILII"},
    {350,"TRILII"},
    {351,"TRILII"},
    {352,"TRILII"},
    {353,"TRILII"},
    {354,"TRILII"},
    {355,"TRILII"},
    {356,"TRILII"},
    {357,"TRILII"},
    {358,"TRILII"},
    {359,"TRILII"},
    {360,"TRILII"},
    {361,"TRILII"},
    {362,"TRILII"},
    {363,"TRILII"},
    {364,"TRILII"},
    {365,"TRIII"},
    {366,"TRIII"},
    {400,"TRI"},
    {401,"TRII"},
    {402,"TRII"},
    {403,"TRII"},
    {4160,"TRII"},
    {4161,"TRI"},
    {4162,"TRI"},
    {4416,""},
    {4417,"RL"},
    {4418,"TRLII"},
    {4419,"TRII(I)"},
    {4420,"TRII(I)"},
    {4421,"TRI"},
    {4422,"TRI"},
    {4423,"TRIII(I)"},
    {4424,"TRIII(I)"},
    {4425,"TRIIL"},
    {4426,"IIIII"},
    {4428,"TRI"},
    {4429,"TRI"},
    {4430,"TRI"},
    {4431,"TRIIII"},
    {4432,"TRII"},
    {4433,"TRIL(I)"},
    {4445,"IIIIIIIIIII"},
    {4446,"II"},
    {4447,"TRI"},
    {4448,""},
    {4449,""},
    {4450,"TRIIL"},
    {4451,"TRIIL"},
    {4452,"TRIIL"},
    {4453,"TRIIIL"},
    {4454,"TRIIIL"},
    {4455,"TRIIIL"},
    {4456,"RIIIII"},
    {4457,"TRIIIA"},
    {4458,"IIIIA"},
    {4459,"TRIIIL"},
    {4460,"TRI"},
    {4461,"TRI"},
    {4462,"TRI"},
    {4463,"TRI"},
    {4472,"R"},
    {4473,"IIIIIIII"},
    {4474,"I"},
    {4475,"II"},
    {4476,"I"},
    {4477,"TRI"},
    {4479,"TRII"},
    {4480,"TRIII"},
    {4481,"TRIII"},
    {4482,"TRIIIII"},
    {4483,"TRIIIII"},
    {4500,"TRIIIII"},
    {4501,"TRIIIII"},
    {4502,"TRIIIII"},
    {4503,"TRIIIII"},
    {5000,"TRILI"},
    {5001,"TRILI"},
    {5002,"TRILI"},
    {5003,"TRILI"},
    {5004,"TRILI"},
    {5005,"TRILI"},
    {5006,"TRILI"},
    {5007,"TRILI"},
    {5011,"TRII"},
    {5012,"TRIII"},
    {5056,"TRI"},
    {5074,"TRIII"},
    {5075,"I"},
    {5076,"RI"},
    {5078,"TRI"},
    {5090,"TRI"},
    {5101,"TRII"},
    {5103,"RS"},
    {5104,"RS"},
    {5110,"TRI"},
    {5111,"TRI"},
    {5249,"IIIIIIIIIIIIII"},
    {5250,"IIIIIIIIIIIII"},
    {5251,"IIIIIII"},
    {5252,"TRI"},
    {5253,"TRI"},
    {5254,"TRI"},
    {5255,"TRI"},
    {5256,"IIIIIIIIIIIII"},
    {5257,"TRI"},
    {5258,"TRI"},
    {5259,"I"},
    {5260,"IIIIIIIIIIII"},
    {5261,"IIIIIIIIIIIII"},
    {5262,"IIIIIIIIIIII"},
    {5263,"IIIIII"},
    {5264,"II"},
    {5265,"TRI"},
    {5266,"II"},
    {5267,"TRI"},
    {5268,"TRI"},
    {5269,"TRI"},
    {5270,"TRI"},
    {5271,"TRI"},
    {5272,"TRI"},
    {5273,"TRI"},
    {5274,"TRI"},
    {5275,"TRI"},
    {5276,"TRI"},
    {5277,"TRI"},
    {5278,"TRI"},
    {5279,"III"},
    {5280,"II"},
    {5281,"R"},
    {5283,"TRIIIIM"},
    {5288,"RII"},
    {5289,"TRIIIIIIIIIIL"},
    {5290,"IIIIIII"},
    {5291,"III"},
    {5292,"TRIIIIIIIIIIIIIL"},
    {5293,"TRI"},
    {5294,"IIII"},
    {5295,"II"},
    {5296,"TRI"},
    {5299,"II"},
    {5300,"TRIIIII"},
    {5301,"TRIIIII"},
    {5302,"TRIIA"},
    {5303,"IIIA"},
    {5334,"TRII"},
    {5335,""},
    {5336,""},
    {5337,"IIIIIIIIIII"},
    {5338,"IIIIIIIIIIII"},
    {5339,"IIIIIIIIIIII"},
    {5340,"TRII"},
    {5341,"R"},
    {5344,"II"},
    {5345,"TRII"},
    {5346,"TRI"},
    {5358,"RIIII"},
    {5359,"TRIIIA"},
    {5360,"IIIIA"},
    {5361,"TRIII"},
    {5362,"TRI"},
    {5364,""},
    {5365,""},
    {5366,"TRILI"},
    {5367,"TRIIIAL"},
    {5368,"IIIAL"},
    {5369,"TRII(I)"},
    {5370,"RII"},
    {5371,"RII(I)"},
    {5372,"TR"},
    {5373,"TRI(I)"},
    {5374,"TRI(I)"},
    {5375,"TRI(I)"},
    {5376,"TRII"},
    {5377,"TR"},
    {5378,"TRI(I)"},
    {5379,"TRI(I)"},
    {5380,""},
    {5381,"TR"},
    {5382,"TRIIIII"},
    {5384,"TRI(I)"},
    {5390,"TRI"},
    {5391,"TRI"},
    {5392,"TRI"},
    {5393,"TRI"},
    {5394,"TRI"},
    {5395,"TRI"},
    {5396,"TRI"},
    {5397,"L"},
    {5398,"TRIIIIL"},
    {5427,"TRII"},
    {5428,"TRII"},
    {5429,"TRII"},
    {5430,"TRII"},
    {5431,"TRII"},
    {5432,"TRI"},
    {5433,"TRI"},
    {5434,"TRI"},
    {5435,"TRI"},
    {5436,"TRI"},
    {5437,"TRI"},
    {5438,"TRII"},
    {5439,"TRII"},
    {5571,"TRII"},
    {5572,"TRIII"},
    {5573,"TRIII"},
    {5574,"TRII"},
    {5575,"TRI"},
    {5576,"II"},
    {5577,"TRII"},
    {5578,"III"},
    {5580,"TRIIII"},
    {5581,"IIIII"},
    {5585,"TRI"},
    {5586,"TRI"},
    {5587,"TRII"},
    {5588,"TRII"},
    {5589,"TRII"},
    {5590,"TRII"},
    {5591,"TRII"},
    {5592,"TRII"},
    {5593,"TRII"},
    {5594,"TRII"},
    {5595,"TRII"},
    {5596,"TRII"},
    {5597,"TRII"},
    {5598,"TRII"},
    {5600,"TRI"},
    {5601,"TR(I)"},
    {5609,"RS"},
    {5610,"TRIISS"},
    {5611,"TRI(I)"},
    {5614,"TRIIII"},
    {5615,"TRIIII"},
    {5630,"I"},
    {5631,"TRII"},
    {5632,"IL"},
    {5633,"ILL"},
    {5699,"TRII"},
    {5700,"RI"},
    {5701,"R"},
    {5702,"R"},
    {5703,"R"},
    {5704,"R"},
    {5705,"R"},
    {5706,"R"},
    {5707,"R"},
    {5708,"R"},
    {5709,"R"},
    {5710,"R"},
    {5711,"R"},
    {5712,"R"},
    {5713,"TRII"},
    {5714,"TRII"},
    {5715,"TRII"},
    {5716,"TRII"},
    {5717,"TRII"},
    {5718,"TRII"},
    {5719,"TRII"},
    {5720,"TRII"},
    {5721,"TR"},
    {5722,"TR"},
    {5723,"TR"},
    {5724,"TRIIII"},
    {5725,"TRII"},
    {5726,"TR"},
    {5727,"TR"},
    {5728,"TRI"},
    {5729,"TRII"},
    {5730,"TRII"},
    {5731,"TRIII"},
    {5732,"TRI"},
    {5733,"TRI"},
    {5734,"TRI"},
    {5735,"TRI"},
    {5736,"TRI"},
    {5737,"TRI"},
    {5738,"TRI"},
    {5739,"TRI"},
    {5740,"TRI"},
    {5741,"TRI"},
    {5742,"TRI"},
    {5743,"TRI"},
    {5744,"TRI"},
    {5745,"TRI"},
    {5746,"TRIII"},
    {5747,"TRIII"},
    {5748,"TRIII"},
    {5749,"TRIIII"},
    {5750,"TRII"},
    {5751,"TRIIII"},
    {5752,"TRI"},
    {5753,"TRII"},
    {5754,"TRI"},
    {5755,"TRII"},
    {5756,"TRII"},
    {5757,"TRIII"},
    {5758,"TRIIII"},
    {5759,"TRIIII"},
    {5760,"TRIIIII"},
    {5761,"TRIII"},
    {5762,"TRIIII"},
    {5763,"TRIIII"},
    {5764,"TRIIIII"},
    {5765,"TRI"},
    {5766,"TRI"},
    {5767,"TRI"},
    {5768,"TRI"},
    {5769,"TRI"},
    {5770,"TRII"},
    {5771,"TRII"},
    {5772,"TRII"},
    {5773,"TRIII"},
    {5774,"TRIII"},
    {5775,"TRIII"},
    {5776,"TRII"},
    {5777,"TRI"},
    {5778,"TRI"},
    {5779,"TRI"},
    {5780,"TRI"},
    {5781,"TRIIIIIII"},
    {5782,"TRIIIIIIII"},
    {5783,"TRI"},
    {5784,"TRI"},
    {5785,"TRI"},
    {5786,"TRIII"},
    {5787,"TRIIII"},
    {5788,"TRIII"},
    {5789,"TRIIII"},
    {5790,"TRI"},
    {5791,"TRI"},
    {5792,"TRIIIIII"},
    {5793,"TRIIIIIIII"},
    {5794,"TRIIIIIIIIIII"},
    {5795,"TRII"},
    {5796,"TRI"},
    {5797,"TRII"},
    {5798,"TRIIII"},
    {5799,"TRII"},
    {5800,"TRI"},
    {5801,"TRII"},
    {5802,"TRII"},
    {5803,"TRII"},
    {5804,"TRIII"},
    {5805,"TRIIII"},
    {5806,"TRIII"},
    {5807,"TRIIII"},
    {5808,"TRI"},
    {5809,"TRI"},
    {5810,"TRI"},
    {5811,"TRI"},
    {5812,"TRI"},
    {5813,"TRI"},
    {5814,"TRI"},
    {5815,"TRI"},
    {5816,"TRI"},
    {5818,"TRI"},
    {5819,"TR"},
    {5820,"I"},
    {5840,"TRILLLLLL"},
    {5841,"TRILLLLL"},
    {5842,"TRILLLLL"},
    {5843,"TRILLLL"},
    {5846,"TRILILLLLL"},
    {5847,"TRILILLLLL"},
    {5848,"TRILILLLLL"},
    {5849,"TRILILLLLL"},
    {5850,"TRILIL"},
    {5851,"TRILIL"},
    {5852,"TRILIL"},
    {5853,"TRILIL"},
    {5854,"TRILIL"},
    {5855,"TRILLLLL"},
    {5856,"TRILLLLL"},
    {5857,"TRILLLLL"},
    {5858,"TRILILLLLL"},
    {5859,"TRILLLLL"},
    {5860,"TRILLLLL"},
    {5861,"TRILLLLL"},
    {5862,"TRILLLLL"},
    {5863,"TRILLLLL"},
    {5864,"TRILLLLL"},
    {5865,"TRILLLLL"},
    {5866,"TRILLLLL"},
    {5867,"TRILLLLL"},
    {5868,"TRILLLLL"},
    {5869,"TRILLLLL"},
    {5870,"TRILLLLL"},
    {5871,"TRILLLLL"},
    {5872,"TRILLLLL"},
    {5873,"TRILLLLL"},
    {5874,"TRILLLLL"},
    {5875,"TRILLLLL"},
    {5876,"TRILLLLL"},
    {5877,"TRILLLLL"},
    {5878,"TRILLLLL"},
    {5879,"TRILILLLLL"},
    {5880,"TRILILLLLL"},
    {5881,"TRILILLLLL"},
    {5882,"TRILILLLL"},
    {5887,"(L)"},
    {5911,"RI"},
    {5912,"RII"},
    {5913,"R(I)"},
    {5923,"TRIILLLLL"},
    {5924,"TRIILLLLL"},
    {5925,"TRIILLLLL"},
    {5926,"TRIILLLLL"},
    {5927,"TRIILLLLL"},
    {5928,"TRIILLLLL"},
    {5929,"TRIILLLLL"},
    {5930,"TRIILLLLL"},
    {5931,"TRIILLLLL"},
    {5932,"TRIILLLLL"},
    {5933,"TRIILLLLL"},
    {5934,"TRI"},
    {5938,"TRI"},
    {5946,"TRII"},
    {5947,"TRII"},
    {5949,"TRII"},
    {6016,"TRI"},
    {6017,"TRI"},
    {6018,"TRII"},
    {6019,"TRII"},
    {6020,"TRII"},
    {6021,"TRII"},
    {6022,"TRII"},
    {6023,"TRII"},
    {6024,"TRII"},
    {6025,"TRII"},
    {6026,"TRI"},
    {6027,"TRII"},
    {6028,"TRII"},
    {6029,"TRI"},
    {6030,"TRI"},
    {6031,"TRII"},
    {6032,"TRII"},
    {6035,"TRIIII"},
    {6086,"RL"},
    {6090,"(I)"},
    {6091,"(I)"},
    {6092,"(I)"},
    {6096,"TR(I)"},
    {6116,"TRI"},
    {6117,"TRI"},
    {6142,"III"},
    {6143,"III"},
    {6145,"TRI"},
    {6221,"IIA"},
    {6231,"IIIIIIIIII"},
    {6232,"IIIIIIIIII"},
    {6233,"IIIIIIIIII"},
    {6234,"IIIIIIIII"},
    {6235,"IIIIIIIIII"},
    {6237,"TRIIIIL"},
    {6401,"TRILI"},
    {6402,"TRILI"},
    {6403,"TRILI"},
    {6404,"TRILI"},
    {6405,"TRILI"},
    {6406,"TRILI"},
    {6407,"TRILI"},
    {6408,"TRILI"},
    {6428,"TRILII"},
    {6429,"IILI"},
};

    inline const char* KindsFor(uint32_t op) {
        size_t lo = 0, hi = sizeof(kSpvOps) / sizeof(kSpvOps[0]);
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (kSpvOps[mid].op < op) lo = mid + 1; else hi = mid;
        }
        if (lo < sizeof(kSpvOps) / sizeof(kSpvOps[0]) && kSpvOps[lo].op == op) return kSpvOps[lo].kinds;
        return nullptr;
    }

    // Calls f(wordIndex, role) for every id operand of an instruction. role: 'T' result type, 'R' result id, 'I' id use.
    template<class F> void ForEachId(const uint32_t* w, uint32_t op, uint32_t wc, F&& f) {
        const char* k = KindsFor(op);
        if (!k) return;
        size_t pos = 1;
        auto token = [&](char t) {
            if (pos >= wc) return;
            switch (t) {
            case 'T': f(pos, 'T'); ++pos; break;
            case 'R': f(pos, 'R'); ++pos; break;
            case 'I': f(pos, 'I'); ++pos; break;
            case 'L': ++pos; break;
            case 'N': pos = wc; break;
            case 'S': {
                while (pos < wc) {
                    const uint32_t x = w[pos++];
                    if ((x & 0xFF) == 0 || (x & 0xFF00) == 0 || (x & 0xFF0000) == 0 || (x & 0xFF000000) == 0) break;
                }
                break;
            }
            case 'M': {
                ++pos;                                 // image operand mask
                while (pos < wc) { f(pos, 'I'); ++pos; }
                break;
            }
            case 'A': {
                const uint32_t mask = w[pos++];
                if (mask & 0x2) ++pos;                 // Aligned literal
                if ((mask & 0x8) && pos < wc) { f(pos, 'I'); ++pos; }
                if ((mask & 0x10) && pos < wc) { f(pos, 'I'); ++pos; }
                break;
            }
            default: break;
            }
        };
        for (size_t i = 0; k[i];) {
            if (k[i] == '(') {
                size_t j = i + 1;
                while (k[j] && k[j] != ')') ++j;
                while (pos < wc) {
                    const size_t before = pos;
                    for (size_t c = i + 1; c < j; ++c) token(k[c]);
                    if (pos == before) break;
                }
                i = j + 1;
            } else { token(k[i]); ++i; }
        }
    }


    inline bool IsRayTracingCapability(uint32_t cap) {
        return cap == 4472 || cap == 4479 || cap == 5340 || cap == 5341 || cap == 5342;
    }

    inline bool IsRayTracingOpForSignature(uint32_t op) {
        // OpTypeAccelerationStructureKHR / OpTypeRayQueryKHR
        if (op == 5341 || op == 4472) return true;
        // OpTraceRayKHR, OpExecuteCallableKHR, OpConvertUToAccelerationStructureKHR
        if (op == 4445 || op == 4446 || op == 4447) return true;
        // RayQueryKHR instructions (4473..4479)
        if (op >= 4473 && op <= 4479) return true;
        // OpRayQueryGetRayTMinKHR / GetRayFlagsKHR / GetWorldRayOriginKHR / GetWorldRayDirectionKHR
        if (op >= 6016 && op <= 6032) return true;
        return false;
    }

    // True for instructions that describe a module's interface / declarations.
    // Their enum values (formats, builtin numbers, execution modes, decorator
    // numbers) vary between driver-capability profiles, so they are reduced to
    // "shape" only. Function bodies are kept verbatim (minus ids).
    inline bool IsDeclarationOpForSignature(uint32_t op) {
        switch (op) {
        case 10:  // OpExtension
        case 11:  // OpExtInstImport
        case 12:  // OpExtInst
        case 15:  // OpEntryPoint
        case 16:  // OpExecutionMode
        case 17:  // OpCapability
        case 19:  // OpTypeVoid
        case 20:  // OpTypeBool
        case 21:  // OpTypeInt
        case 22:  // OpTypeFloat
        case 23:  // OpTypeVector
        case 24:  // OpTypeMatrix
        case 25:  // OpTypeImage
        case 26:  // OpTypeSampler
        case 27:  // OpTypeSampledImage
        case 28:  // OpTypeArray
        case 29:  // OpTypeRuntimeArray
        case 30:  // OpTypeStruct
        case 31:  // OpTypeOpaque
        case 32:  // OpTypePointer
        case 33:  // OpTypeFunction
        case 71:  // OpDecorate
        case 72:  // OpMemberDecorate
        case 73:  // OpDecorationGroup
        case 74:  // OpGroupDecorate
        case 75:  // OpGroupMemberDecorate
        case 331: // OpDecorateId
        case 5632: // OpDecorateString
        case 5633: // OpMemberDecorateString
            return true;
        default:
            return false;
        }
    }

    // Structural signature: same key <=> same program shape on any GPU.
    inline std::string StructuralSignature(const uint32_t* w, size_t n, size_t* instructionCount = nullptr,
                                           bool* hasRayTracing = nullptr) {
        std::string sig;
        if (instructionCount) *instructionCount = 0;
        if (hasRayTracing) *hasRayTracing = false;
        if (!w || n < 5 || w[0] != 0x07230203u) return sig;

        std::unordered_map<uint32_t, uint32_t> idMap;   // original id -> stable id
        uint32_t nextId = 1;
        auto mapId = [&](uint32_t id) -> uint32_t {
            if (id == 0) return 0;
            auto it = idMap.find(id);
            if (it != idMap.end()) return it->second;
            idMap[id] = nextId;
            return nextId++;
        };

        sig.reserve(n * 3);
        char buf[48];

        // ---- main pass -------------------------------------------------------
        for (size_t i = 5; i < n;) {
            const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
            if (wc == 0 || i + wc > n) break;

            if (IsRayTracingOpForSignature(op)) {
                if (hasRayTracing) *hasRayTracing = true;
                if (instructionCount) ++*instructionCount;
                i += wc;
                continue;                               // ignore RT-only code entirely
            }
            if (op == 10 || op == 11) { i += wc; continue; }   // extension/import strings
            if (op == 17 && wc >= 2 && IsRayTracingCapability(w[i + 1])) {
                // OpCapability RayQueryKHR / RayTracingKHR / RayTracingNV: the module
                // declares RT support. Ignored here so an RT-enabled variant of a
                // shader still matches its RT-free replacement.
                if (hasRayTracing) *hasRayTracing = true;
                i += wc;
                continue;
            }
            sprintf_s(buf, sizeof(buf), "|%u:%u", op, wc);
            sig += buf;
            if (op == 15 && wc >= 2) {
                // OpEntryPoint: the execution model is identical on every GPU, so it
                // stays part of the key (a fragment shader must never match a compute
                // replacement). Ray-tracing stages have no equivalent, so they are
                // never matched at all.
                const uint32_t model = w[i + 1];
                if (model >= 5313 && model <= 5321) return std::string();
                sprintf_s(buf, sizeof(buf), "E%u", model);
                sig += buf;
            }

            if (op == 42 || op == 43 || op == 44 || op == 45) {
                // OpConstant/OpConstantTrue/OpConstantFalse/OpConstantComposite:
                // ids may embed 16-bit or 64-bit encodings that differ by profile.
                // Keep the type only, drop the literal payload.
                sig += "#T";
                ForEachId(w + i, op, wc, [&](size_t idx, char role) {
                    if (role == 'T') { sig += std::to_string(mapId(w[i + idx])); }
                });
            } else if (IsDeclarationOpForSignature(op)) {
                // Keep ids (mapped) and drop every enum / literal number.
                ForEachId(w + i, op, wc, [&](size_t idx, char role) {
                    if (role == 'T' || role == 'R' || role == 'I') {
                        sig += role;
                        sig += std::to_string(mapId(w[i + idx]));
                    } else {
                        sig += 'L';                     // literal placeholder
                    }
                });
            } else if (op == 59) {
                // OpVariable: storage class matters (interface), type/result ids do not.
                sig += "#S";
                ForEachId(w + i, op, wc, [&](size_t idx, char role) {
                    if (role == 'T' || role == 'R') { sig += role; sig += std::to_string(mapId(w[i + idx])); }
                    else sig += 'L';
                });
            } else {
                // Function body / control flow: ids and width-bearing literals only.
                ForEachId(w + i, op, wc, [&](size_t idx, char role) {
                    if (role == 'T' || role == 'R' || role == 'I') {
                        sig += std::to_string(mapId(w[i + idx]));
                        sig += ',';
                    }
                });
            }

            if (instructionCount) ++*instructionCount;
            i += wc;
        }

        return sig;
    }

    // Universal replacement lookup.
    //
    // Why the byte hash and even the order-sensitive structural signature are not
    // enough: every machine can receive a different build of the same shader. The
    // log from a GTX 1080 Ti shows the 3 replacement files never matching while the
    // game supplies 10 different modules for the same material set. A mismatch
    // cannot be recovered from the bytes alone, so the lookup degrades step by step,
    // each step being strictly typed so a replacement for another shader is never
    // used:
    //
    //   exact     - FNV-1a of the bytes the game supplied (reference machine)
    //   names/plan- identical entry point + debug names + descriptor interface
    //   identical - same instruction multiset and same interface
    //   similar   - same shader with a small build difference
    //   loose     - same shader with heavy divergence (different capability profile,
    //               ray-tracing code lowered on one side only)
    //
    // The decisive signal is the instruction multiset (order independent!) and the
    // debug names, which come from the shader source and are therefore identical on
    // every GPU. The execution model is always part of the comparison, so a vertex
    // shader can never be replaced by a fragment or compute module.
    // -----------------------------------------------------------------------

    // Everything that identifies "the same shader" without depending on the exact
    // bytes a particular driver-capability profile produced.
    struct ReplacementFingerprint {
        uint32_t stage = 0xFFFFFFFFu;
        std::string norm;
        std::string names;
        std::string layout;
        std::map<uint64_t, uint32_t> shapeHistogram;   // instruction shape -> count
        size_t words = 0;
        size_t interfaceCount = 0;
    };

    // Cosine similarity of two integer histograms, in [0,1]. Independent of ids, of
    // the instruction order and of the module length.
    inline double HistogramSimilarity(const std::map<uint64_t, uint32_t>& a,
                                     const std::map<uint64_t, uint32_t>& b) {
        if (a.empty() || b.empty()) return 0.0;
        double dot = 0.0, na = 0.0, nb = 0.0;
        for (const auto& kv : a) {
            na += double(kv.second) * double(kv.second);
            auto it = b.find(kv.first);
            if (it != b.end()) dot += double(kv.second) * double(it->second);
        }
        for (const auto& kv : b) nb += double(kv.second) * double(kv.second);
        if (na <= 0.0 || nb <= 0.0) return 0.0;
        return dot / (std::sqrt(na) * std::sqrt(nb));
    }

    // Splits a "name1|name2|" list into a set.
    inline void SplitNames(const std::string& s, std::unordered_set<std::string>& out) {
        size_t start = 0;
        while (start < s.size()) {
            size_t end = s.find('|', start);
            if (end == std::string::npos) end = s.size();
            if (end > start) out.insert(s.substr(start, end - start));
            start = end + 1;
        }
    }

    // Jaccard overlap of two interface-layout key strings.
    inline double LayoutSimilarity(const std::string& a, const std::string& b) {
        if (a.empty() && b.empty()) return 1.0;
        std::unordered_set<std::string> sa, sb;
        SplitNames(a, sa);   // layout keys are also ';'-separated; split below
        sa.clear(); sb.clear();
        auto split = [](const std::string& s, char sep, std::unordered_set<std::string>& out) {
            size_t start = 0;
            while (start < s.size()) {
                size_t end = s.find(sep, start);
                if (end == std::string::npos) end = s.size();
                if (end > start) out.insert(s.substr(start, end - start));
                start = end + 1;
            }
        };
        split(a, ';', sa); split(b, ';', sb);
        if (sa.empty() && sb.empty()) return 1.0;
        size_t inter = 0;
        for (const std::string& k : sa) if (sb.count(k)) ++inter;
        const size_t uni = sa.size() + sb.size() - inter;
        return uni ? double(inter) / double(uni) : 0.0;
    }

    // Extracts a SPIR-V literal string (OpName / OpExtension / OpExtInstImport).
    inline std::string LiteralString(const uint32_t* w, uint32_t wc) {
        std::string out;
        for (uint32_t k = 0; k < wc; ++k) {
            for (unsigned b = 0; b < 4; ++b) {
                const char c = static_cast<char>((w[k] >> (b * 8)) & 0xFFu);
                if (c == 0) return out;
                out.push_back(c);
            }
        }
        return out;
    }

    inline bool ExtractFingerprint(const uint32_t* w, size_t n, ReplacementFingerprint& fp) {
        fp = ReplacementFingerprint{};
        if (!w || n < 5 || w[0] != 0x07230203u) return false;
        fp.words = n;
        fp.norm = spvrw::StructuralSignature(w, n);

        std::map<uint32_t, uint32_t> setOf, bindingOf, locationOf;
        std::vector<std::string> nameList;
        std::vector<std::pair<uint32_t, uint32_t>> variables;   // resultId, storageClass

        for (size_t i = 5; i < n;) {
            const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
            if (wc == 0 || i + wc > n) break;
            const uint32_t* b = w + i;
            if (op == 15 && wc >= 3 && fp.stage == 0xFFFFFFFFu) {        // OpEntryPoint
                fp.stage = b[1];
            } else if (op == 5 && wc >= 3) {                            // OpName
                nameList.push_back(LiteralString(b + 2, wc - 2));
            } else if (op == 6 && wc >= 4) {                            // OppMemberName
                nameList.push_back(LiteralString(b + 3, wc - 3));
            } else if (op == 71 && wc >= 4) {                           // OpDecorate
                const uint32_t target = b[1], dec = b[2];
                if (dec == 34) setOf[target] = b[3];                    // DescriptorSet
                else if (dec == 33) bindingOf[target] = b[3];           // Binding
                else if (dec == 30) locationOf[target] = b[3];          // Location
            } else if (op == 59 && wc >= 4) {                           // OpVariable
                variables.push_back({ b[2], b[3] });
            }

            // Order-independent structural shape of this instruction: the opcode
            // plus, for every operand, whether it is an id ("I") or a literal ("L").
            // Ids and literal values are dropped, so the same shader built by a
            // different frontend or for a different capability profile keeps the same
            // set of shapes. Ray-tracing code is excluded because our own lowering may
            // remove it on one machine and not on another.
            const bool rtOp = spvrw::IsRayTracingOpForSignature(op);
            const bool rtCap = (op == 17 && wc >= 2 && spvrw::IsRayTracingCapability(w[i + 1]));
            // Decorations are frontend metadata, not structure: some drivers emit
            // RelaxedPrecision/NoContraction for every single float operation, others
            // none at all. Counting them made two builds of the SAME shader score only
            // ~0.82 and fail a match that had nothing to do with the shader.
            const bool metaOp = (op == 71 || op == 72 || op == 73 || op == 74);
            if (!rtOp && !rtCap && !metaOp) {
                std::vector<char> roles(wc, 'L');
                spvrw::ForEachId(w + i, op, wc, [&](size_t idx, char) { if (idx < roles.size()) roles[idx] = 'I'; });
                uint64_t h = 0xcbf29ce484222325ull;
                auto mixByte = [&](uint8_t byte) { h ^= byte; h *= 0x100000001b3ull; };
                mixByte(static_cast<uint8_t>(op & 0xFF));
                mixByte(static_cast<uint8_t>(op >> 8));
                for (uint32_t k = 1; k < wc; ++k) mixByte(static_cast<uint8_t>(roles[k]));
                ++fp.shapeHistogram[h];
            }
            i += wc;
        }

        std::sort(nameList.begin(), nameList.end());
        nameList.erase(std::unique(nameList.begin(), nameList.end()), nameList.end());
        for (const std::string& nm : nameList) { fp.names += nm; fp.names += '|'; }

        std::vector<std::string> keys;
        keys.reserve(variables.size());
        for (const auto& v : variables) {
            const uint32_t resultId = v.first, storageClass = v.second;
            if (storageClass == 6 || storageClass == 7) continue;   // Private / Function
            char key[96];
            auto sit = setOf.find(resultId), bit = bindingOf.find(resultId);
            auto lit = locationOf.find(resultId);
            if (sit != setOf.end() || bit != bindingOf.end()) {
                _snprintf_s(key, sizeof(key), _TRUNCATE, "D%u:%u:%u",
                    storageClass,
                    sit != setOf.end() ? sit->second : 0xFFFFFFFFu,
                    bit != bindingOf.end() ? bit->second : 0xFFFFFFFFu);
            } else {
                _snprintf_s(key, sizeof(key), _TRUNCATE, "L%u:%u",
                    storageClass,
                    lit != locationOf.end() ? lit->second : 0xFFFFFFFFu);
            }
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        fp.interfaceCount = keys.size();
        for (const std::string& k : keys) { fp.layout += k; fp.layout += ';'; }
        return !fp.norm.empty();
    }

    inline const char* SpvStageName(uint32_t model) {
        switch (model) {
        case 0: return "vertex";
        case 1: return "tess-control";
        case 2: return "tess-eval";
        case 3: return "geometry";
        case 4: return "fragment";
        case 5: return "compute";
        case 5267: return "task";
        case 5268: return "mesh";
        default: return "other";
        }
    }

    // Portable keys: the descriptor interface (which the pipeline layout requires
    // anyway) plus the debug names. Instruction order is never part of a key.
    struct ReplacementKeySet {
        std::string names;
        std::string plan;
        bool valid = false;
        uint32_t stage = 0xFFFFFFFFu;
    };

    inline ReplacementKeySet MakeKeySet(const spvrw::ReplacementFingerprint& fp) {
        ReplacementKeySet k;
        if (fp.norm.empty()) return k;
        k.valid = true;
        k.stage = fp.stage;
        char buf[64];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u#", fp.stage);
        k.plan = buf;  k.plan += fp.layout;
        k.names = k.plan;  k.names += '#'; k.names += fp.names;
        return k;
    }

    struct ReplacementEntry {
        std::wstring path;
        uint32_t stage = 0xFFFFFFFFu;
        spvrw::ReplacementFingerprint fp;
    };

    struct ReplacementIndex {
        std::unordered_map<std::string, std::wstring> names;
        std::unordered_map<std::string, std::wstring> plan;
        std::unordered_set<std::string> ambNames, ambPlan;
        std::unordered_map<uint64_t, std::wstring> learned;   // hash -> file
        std::vector<ReplacementEntry> entries;
        size_t files = 0, unreadable = 0;
        bool ready = false;
    };

    // kFuzzyMargin keeps two equally plausible candidates from being swapped. When
    // names are available the margin is generous (the ranking is trustworthy). When
    // one side has no debug names the name gate is skipped, the scores of similar
    // shaders sit very close to each other, and a strict margin would reject the
    // correct file; there a relative margin is used instead. Rejecting everything
    // would leave the original module in place, which is the black-screen failure
    // this code exists to remove.
    constexpr double kFuzzyMargin = 0.05;
    // The margin is in score units (shape + layout + names, max 3.0). Different drivers
    // put the same shader within ~0.02 of each other and different shaders far apart, so
    // a very small margin is enough once the absolute floors below are respected.
    constexpr double kFuzzyMarginNoNames = 0.0005;
    // Without names the score is a pure instruction-shape comparison, which two
    // unrelated shaders that merely share a similar mix can also reach. A very high
    // absolute floor is therefore required as well, so a name-less module is only
    // ever mapped onto a nearly identical module.
    // A vendor's frontend can add or drop one or two instructions for the same shader
    // (a precision decoration, an execution mode), so requiring 0.99 here rejected
    // modules that differ by exactly that and nothing else.
    constexpr double kFuzzyShapeFloorNoNames = 0.97;

    ReplacementIndex* g_replIndex = nullptr;
    ReplacementIndex* EnsureReplacementIndex();
    SRWLOCK g_replIndexLock = SRWLOCK_INIT;
    volatile LONG g_replIndexState = 0;
    volatile LONG g_replHitsExact = 0, g_replHitsSimilar = 0, g_replMisses = 0;

    void AddToIndex(std::unordered_map<std::string, std::wstring>& m,
                    std::unordered_set<std::string>& ambiguous,
                    const std::string& key, const std::wstring& path) {
        if (key.empty()) return;
        auto it = m.find(key);
        if (it == m.end()) { m.emplace(key, path); return; }
        if (it->second != path) ambiguous.insert(key);   // never guess between two shaders
    }

    // Chooses the replacement for one incoming module by similarity. The tiers go
    // from strict to loose:
    //
    //   identical - same instruction multiset and descriptor interface
    //   similar   - same shader after a small build difference
    //   loose     - same shader with heavy divergence (different capability profile,
    //               ray-tracing code lowered on one side only)
    //
    // Debug names are the decisive signal: they come from the shader source, so they
    // are identical for the same shader on every GPU and different for a different
    // shader even when the descriptor interface coincides. When one of the modules
    // carries no names at all (stripped build) the name gate is skipped instead of
    // failing the match. The best candidate must also beat the runner-up by
    // kFuzzyMargin, otherwise nothing is picked.
    inline const std::wstring* BestSimilarityMatch(const std::vector<ReplacementEntry>& entries,
                                                  const ReplacementFingerprint& fp,
                                                  const char** matchedLevel) {
        if (matchedLevel) *matchedLevel = nullptr;
        struct Tier { double shape, layout, names; const char* name; };
        const Tier tiers[] = {
            { 0.980, 1.00, 1.00, "identical" },
            { 0.900, 0.75, 0.50, "similar" },
            { 0.750, 0.50, 0.34, "loose" },
        };
        for (const Tier& tier : tiers) {
            double best = -1.0, second = -1.0, bestShape = 0.0;
            const std::wstring* bestPath = nullptr;
            for (const ReplacementEntry& e : entries) {
                if (e.stage != fp.stage) continue;
                const double shape = HistogramSimilarity(fp.shapeHistogram, e.fp.shapeHistogram);
                if (shape < tier.shape) continue;
                const double lay = LayoutSimilarity(fp.layout, e.fp.layout);
                if (lay < tier.layout) continue;

                std::unordered_set<std::string> mine, theirs;
                SplitNames(fp.names, mine);
                SplitNames(e.fp.names, theirs);
                double namesScore = 0.0;
                if (!mine.empty() && !theirs.empty()) {
                    size_t inter = 0;
                    for (const std::string& nm : mine) if (theirs.count(nm)) ++inter;
                    const size_t uni = mine.size() + theirs.size() - inter;
                    namesScore = uni ? double(inter) / double(uni) : 0.0;
                    if (namesScore < tier.names) continue;
                }

                const double score = shape + lay + namesScore;
                if (score > best) { second = best; best = score; bestShape = shape; bestPath = &e.path; }
                else if (score > second) { second = score; }
            }
            // The margin depends on whether the names of the winner were compared:
            // when the incoming module has none (stripped build) every score is a pure
            // shape comparison and similar shaders land within a few thousandths.
            const bool nameless = fp.names.empty();
            const double margin = nameless ? kFuzzyMarginNoNames : kFuzzyMargin;
            const bool shapeHighEnough = !nameless || bestShape >= kFuzzyShapeFloorNoNames;
            if (bestPath && shapeHighEnough && (second < 0.0 || best - second >= margin)) {
                if (matchedLevel) *matchedLevel = tier.name;
                return bestPath;
            }
        }
        return nullptr;
    }


    inline void RenumberAndShuffle(const uint32_t* w, size_t n, uint32_t seed, bool dropNames,
                                   std::vector<uint32_t>& out) {
        out.clear();
        if (!w || n < 5) return;

        struct Inst { uint32_t op, wc; size_t off; };
        std::vector<Inst> insts;
        for (size_t i = 5; i < n;) {
            const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
            if (wc == 0 || i + wc > n) break;
            if (dropNames && (op == 5 || op == 6)) { i += wc; continue; }   // OpName / OppMemberName
            insts.push_back({ op, wc, i });
            i += wc;
        }

        // Split into the declaration section (must stay ordered: types refer to
        // earlier declarations) and the function section, whose instruction order
        // inside a block is free. Another frontend legitimately emits that part in a
        // different order, which is exactly what used to defeat the old matching.
        std::vector<Inst> head, funcs, tail;
        size_t fBegin = insts.size();
        for (size_t k = 0; k < insts.size(); ++k) {
            if (insts[k].op == 54) { fBegin = k; break; }        // OpFunction
        }
        size_t fEnd = fBegin;
        if (fBegin < insts.size()) {
            while (fEnd < insts.size() && insts[fEnd].op != 56) ++fEnd;   // OpFunctionEnd
            if (fEnd < insts.size()) ++fEnd;
        }
        head.assign(insts.begin(), insts.begin() + static_cast<long>(fBegin));
        if (fBegin < insts.size()) funcs.assign(insts.begin() + static_cast<long>(fBegin), insts.begin() + static_cast<long>(fEnd));
        if (fEnd < insts.size()) tail.assign(insts.begin() + static_cast<long>(fEnd), insts.end());

        // Deterministic PRNG so a failure can be reproduced from the log alone.
        uint32_t rng = seed ? seed : 0x9E3779B9u;
        auto next = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };

        // Shuffle only the run between a block label and its terminator: that is the
        // part whose order is free, so the module stays structurally sensible while
        // the instruction order really changes. Order must not matter for matching.
        size_t labelAt = funcs.size();
        for (size_t k = 0; k < funcs.size(); ++k) if (funcs[k].op == 248) { labelAt = k; break; }
        if (labelAt + 1 < funcs.size()) {
            size_t bodyEnd = labelAt + 1;
            while (bodyEnd < funcs.size() && funcs[bodyEnd].op != 253 && funcs[bodyEnd].op != 56 &&
                   funcs[bodyEnd].op != 249 && funcs[bodyEnd].op != 250) ++bodyEnd;
            for (size_t k = bodyEnd; k > labelAt + 1; --k)
                std::swap(funcs[k - 1], funcs[labelAt + 1 + (next() % (k - 1 - labelAt))]);
        }

        // A module built for another capability profile also has a different amount
        // of code. Remove a few ordinary body instructions (never the structural ones:
        // OpFunction/OpFunctionEnd/OpLabel/OpBranch/OpReturn), which changes the size
        // and the instruction multiset while keeping the module well formed.
        {
            std::vector<Inst> kept;
            kept.reserve(funcs.size());
            uint32_t budget = 2 + (seed % 5);
            for (const Inst& in : funcs) {
                const bool structural = (in.op == 54 || in.op == 56 || in.op == 248 ||
                                         in.op == 249 || in.op == 250 || in.op == 253 ||
                                         in.op == 252 || in.op == 251);
                if (!structural && budget > 0 && (next() % 11) == 0) { --budget; continue; }
                kept.push_back(in);
            }
            funcs.swap(kept);
        }

        // Emit everything, then rebuild with a fresh, injective id numbering: that is
        // what makes two builds of the same shader differ the most in bytes.
        std::vector<uint32_t> reduced;
        reduced.insert(reduced.end(), w, w + 5);   // SPIR-V header stays intact
        auto emit = [&](const std::vector<Inst>& v) {
            for (const Inst& in : v) for (uint32_t k = 0; k < in.wc; ++k) reduced.push_back(w[in.off + k]);
        };
        emit(head); emit(funcs); emit(tail);
        if (reduced.size() <= 5) return;

        // id remap: original id -> base + sequential number (injective by construction)
        std::unordered_map<uint32_t, uint32_t> idMap;
        uint32_t nextId = 1 + (seed % 4093u) * 7u;
        auto mapId = [&](uint32_t id) -> uint32_t {
            if (id == 0) return 0;
            auto it = idMap.find(id);
            if (it != idMap.end()) return it->second;
            idMap.emplace(id, nextId);
            return nextId++;
        };

        out.assign(reduced.begin(), reduced.begin() + 5);
        for (size_t i = 5; i + 1 < reduced.size();) {
            const uint32_t wc = reduced[i] >> 16, op = reduced[i] & 0xFFFFu;
            if (wc == 0 || i + wc > reduced.size()) break;
            const size_t outStart = out.size();
            for (uint32_t k = 0; k < wc; ++k) out.push_back(reduced[i + k]);
            // OpEntryPoint (15) and OpName/OppMemberName (5/6) carry an id in word 1-2
            // plus string literals; ForEachId knows which operands are ids.
            if (op != 7 && op != 8) {
                spvrw::ForEachId(out.data() + outStart, op, wc, [&](size_t idx, char role) {
                    if (role == 'L' || idx >= wc) return;
                    uint32_t& v = out[outStart + idx];
                    v = mapId(v);
                });
            }
            i += wc;
        }
    }
}

using namespace spvrw;

// ---- file helpers ---------------------------------------------------------
static bool ReadSpvFile(const std::wstring& path, std::vector<uint32_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = false;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 20 && sz.QuadPart < (64ll << 20) && (sz.QuadPart % 4) == 0) {
        out.resize(static_cast<size_t>(sz.QuadPart / 4));
        DWORD got = 0;
        ok = ReadFile(f, out.data(), static_cast<DWORD>(sz.QuadPart), &got, nullptr) && got == sz.QuadPart && out[0] == 0x07230203u;
    }
    CloseHandle(f);
    if (!ok) out.clear();
    return ok;
}

static const char* StageName(uint32_t model) {
    switch (model) {
    case 0: return "vertex";
    case 1: return "tess control";
    case 2: return "tess eval";
    case 3: return "geometry";
    case 4: return "fragment";
    case 5: return "compute";
    case 5267: return "task";
    case 5268: return "mesh";
    default: return "unknown";
    }
}

static size_t CountNames(const std::string& names) {
    size_t n = 0;
    for (char c : names) if (c == '|') ++n;
    return n;
}

// ---- "another vendor's frontend" simulation ---------------------------------
// The standard rebuild above only changes ids, order and debug names. A different
// GPU vendor additionally changes things the driver/frontend decides: the
// OpCapability set, OpSource metadata, the generator word and the bound. Those
// mutations are what a real 1080 Ti / Arc module differs by, so the matcher has to
// survive them as well - otherwise the folder only ever works on the card it was
// captured from.
static void Vendorize(const uint32_t* w, size_t n, uint32_t seed, int profile,
                      std::vector<uint32_t>& out) {
    if (!w || n < 5) return;
    struct Inst { uint32_t op, wc; size_t off; };
    std::vector<Inst> insts;
    for (size_t i = 5; i < n;) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (wc == 0 || i + wc > n) break;
        if (op == 5 || op == 6) { i += wc; continue; }          // debug names
        insts.push_back({ op, wc, i });
        i += wc;
    }

    std::vector<uint32_t> red;
    red.insert(red.end(), w, w + 5);
    // Header: a different vendor writes a different generator id, bound and version.
    red[2] = 0x00080007u + seed;
    red[1] = 0x00010500u;
    for (const Inst& in : insts) {
        if (profile >= 1) {
            // Capability profile: drop every optional capability, then add Int64.
            // That is exactly the difference between a GCN2 card and a Pascal one.
            if (in.op == 17) {
                const uint32_t cap = (in.wc >= 2) ? w[in.off + 1] : 0;
                if (cap != 1) continue;                          // keep only Shader
                for (uint32_t k = 0; k < in.wc; ++k) red.push_back(w[in.off + k]);
                continue;
            }
            if (in.op == 3 || in.op == 2 || in.op == 4) continue; // OpSource/OpSourceExtension/OpSourceContinued
        }
        for (uint32_t k = 0; k < in.wc; ++k) red.push_back(w[in.off + k]);
    }
    if (profile >= 1) {
        // The added capability arrives as a new instruction in the module head.
        const uint32_t capInst = (2u << 16) | 17u;
        red.insert(red.begin() + 5, { capInst, 11u });            // OpCapability Int64
    }
    if (profile >= 2) {
        // A second profile on top: what else two frontends do differently for the same
        // shader source - integer signedness, precision decorations and the execution
        // mode spelling. None of this changes what the shader computes.
        for (size_t i = 5; i < red.size();) {
            const uint32_t wc = red[i] >> 16, op = red[i] & 0xFFFFu;
            if (wc == 0 || i + wc > red.size()) break;
            if (op == 21 && wc >= 4) red[i + 3] = red[i + 3] ^ 1u;       // OpTypeInt signedness
            else if (op == 16 && wc >= 3) red[i + 2] = 17u;             // OpExecutionMode LocalSizeId
            i += wc;
        }
        // OpDecorate RelaxedPrecision on the result of the first few float operations:
        // a real frontend emits these, and they must not break the match.
        std::vector<uint32_t> extra;
        size_t added = 0;
        for (size_t i = 5; i + 2 < red.size() && added < 24;) {
            const uint32_t wc = red[i] >> 16, op = red[i] & 0xFFFFu;
            if (wc == 0 || i + wc > red.size()) break;
            if ((op == 129 || op == 130) && wc >= 3) {                 // OpIAdd / OpFAdd
                extra.push_back((3u << 16) | 71u);                     // OpDecorate
                extra.push_back(red[i + 2]);
                extra.push_back(0u);                                   // RelaxedPrecision
                ++added;
            }
            i += wc;
        }
        if (!extra.empty()) red.insert(red.begin() + 5, extra.begin(), extra.end());
    }

    // Final step: new ids and a new instruction order, like the standard rebuild.
    RenumberAndShuffle(red.data(), red.size(), seed, false, out);
}

// ---- offline comparison of a real module against the folder -----------------
// Prints the exact numbers the in-game lookup compares, so a log from a machine
// where the replacement was not accepted can be explained without guessing.
static void ExplainOne(const wchar_t* file, const std::vector<ReplacementEntry>& entries) {
    std::vector<uint32_t> words;
    if (!ReadSpvFile(file, words)) { printf("  %ls: unreadable\n", file); return; }
    spvrw::ReplacementFingerprint fp;
    if (!spvrw::ExtractFingerprint(words.data(), words.size(), fp)) { printf("  %ls: unparsable\n", file); return; }
    const wchar_t* base = wcsrchr(file, L'\\');
    printf("\n%ls (%s, %zu words, %zu names, %zu ifaces)\n", base ? base + 1 : file,
        StageName(fp.stage), fp.words, CountNames(fp.names), fp.interfaceCount);
    for (const ReplacementEntry& e : entries) {
        const wchar_t* eb = wcsrchr(e.path.c_str(), L'\\');
        const double shape = HistogramSimilarity(fp.shapeHistogram, e.fp.shapeHistogram);
        const double lay = LayoutSimilarity(fp.layout, e.fp.layout);
        printf("   vs %-30ls stage=%s shape=%.3f layout=%.3f\n",
            eb ? eb + 1 : e.path.c_str(), (e.stage == fp.stage) ? "same" : "DIFF", shape, lay);
    }
    const char* level = nullptr;
    const std::wstring* pick = BestSimilarityMatch(entries, fp, &level);
    if (pick) {
        const wchar_t* pb = wcsrchr(pick->c_str(), L'\\');
        printf("   => WOULD MATCH: %ls (%s)\n", pb ? pb + 1 : pick->c_str(), level ? level : "?");
    } else {
        printf("   => NO MATCH (needs shape>=0.98/0.90/0.75 and layout>=1.00/0.75/0.50)\n");
    }
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(65001);
    wchar_t dir[MAX_PATH * 2]{};
    if (argc > 1) {
        wcscpy_s(dir, argv[1]);
    } else {
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (slash) *(slash + 1) = 0;
        wcscat_s(dir, L"spv_replace");
    }
    printf("=== spv_replace portability check ===\n");
    printf("folder: %ls\n\n", dir);
    // Optional second argument: a module captured from the game (DumpLookupInput=N).
    // Then the tool explains, number by number, why it is or is not accepted.
    const bool explainMode = argc > 2;

    std::vector<ReplacementEntry> entries;
    size_t files = 0, unreadable = 0;
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = std::wstring(dir) + L"\\spv_*.spv";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        printf("ERROR: no spv_*.spv files in that folder.\n");
        printf("       Check the path and that files are named spv_<16 hex digits>.spv\n");
        printf("\nPress Enter to close...");
        (void)getchar();
        return 2;
    }
    do {
        ++files;
        const std::wstring path = std::wstring(dir) + L"\\" + fd.cFileName;
        std::vector<uint32_t> words;
        if (!ReadSpvFile(path, words)) {
            ++unreadable;
            printf("  [bad ] %-30ls not readable as SPIR-V (magic 0x07230203, size % 4 == 0)\n", fd.cFileName);
            continue;
        }
        ReplacementFingerprint fp;
        if (!spvrw::ExtractFingerprint(words.data(), words.size(), fp)) {
            ++unreadable;
            printf("  [bad ] %-30ls unparsable module\n", fd.cFileName);
            continue;
        }
        printf("  [ok  ] %-30ls %-14s %6zu words, %2zu descriptors, %2zu names\n",
            fd.cFileName, StageName(fp.stage), fp.words, fp.interfaceCount, CountNames(fp.names));
        ReplacementEntry e;
        e.path = path;
        e.stage = fp.stage;
        e.fp = std::move(fp);
        entries.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    printf("\nread %zu file(s): %zu unusable, %zu usable for matching\n\n",
        files, unreadable, entries.size());
    if (entries.empty()) {
        printf("=== RESULT ===\nFAIL: nothing in this folder can be used.\n");
        printf("\nPress Enter to close...");
        (void)getchar();
        return 3;
    }

    // The real test: rebuild every module the way another driver would and require
    // the matcher to find the original file again.
    size_t pass = 0, fail = 0, namedPass = 0, vendorPass = 0, vendorFail = 0;
    printf("rebuilding each file the way another driver would\n");
    printf("(new ids, shuffled instruction order, stripped names, different size)\n\n");
    for (size_t k = 0; k < entries.size(); ++k) {
        std::vector<uint32_t> words;
        if (!ReadSpvFile(entries[k].path, words)) continue;

        std::vector<uint32_t> stripped, named;
        RenumberAndShuffle(words.data(), words.size(), static_cast<uint32_t>(0x1234u + k), true, stripped);
        RenumberAndShuffle(words.data(), words.size(), static_cast<uint32_t>(0x777u + k), false, named);

        ReplacementFingerprint fa, fb;
        const char* la = nullptr;
        const char* lb = nullptr;
        bool ok1 = false, ok2 = false;
        if (spvrw::ExtractFingerprint(stripped.data(), stripped.size(), fa)) {
            const std::wstring* p = BestSimilarityMatch(entries, fa, &la);
            ok1 = p && *p == entries[k].path;
        }
        if (spvrw::ExtractFingerprint(named.data(), named.size(), fb)) {
            const std::wstring* p = BestSimilarityMatch(entries, fb, &lb);
            ok2 = p && *p == entries[k].path;
        }
        if (ok1) ++pass; else ++fail;
        if (ok2) ++namedPass;

        const wchar_t* name = wcsrchr(entries[k].path.c_str(), L'\\');
        printf("  %-30ls stripped+shuffled: %-6s (%-10s)  named+shuffled: %-6s (%s)\n",
            name ? name + 1 : entries[k].path.c_str(),
            ok1 ? "FOUND" : "MISSED", la ? la : "-",
            ok2 ? "FOUND" : "MISSED", lb ? lb : "-");

        // Same file, but with a different vendor's capability profile and metadata.
        // Several independent variants: one lucky seed proves nothing when the folder
        // holds several shaders of the same stage.
        int vOk = 0, vTotal = 0;
        for (int profile = 1; profile <= 2; ++profile) {
            for (int v = 0; v < 3; ++v) {
                std::vector<uint32_t> foreign;
                Vendorize(words.data(), words.size(),
                    static_cast<uint32_t>(0xBEEFu + k * 37u + v * 101u + profile * 7u), profile, foreign);
                ReplacementFingerprint fc;
                const char* lc = nullptr;
                ++vTotal;
                if (spvrw::ExtractFingerprint(foreign.data(), foreign.size(), fc)) {
                    const std::wstring* p = BestSimilarityMatch(entries, fc, &lc);
                    if (p && *p == entries[k].path) ++vOk;
                }
            }
        }
        vendorPass += vOk; vendorFail += (vTotal - vOk);
        printf("  %-30ls other vendor profile:       %d/%d\n",
            "", vOk, vTotal);
    }

    if (explainMode) {
        printf("\n=== why is this module (not) accepted? ===\n");
        ExplainOne(argv[2], entries);
        printf("\n(Press Enter to close...)");
        (void)getchar();
        return 0;
    }

    printf("\n=== RESULT ===\n");
    printf("rebuilt like another driver: %zu/%zu\n", pass, pass + fail);
    printf("rebuilt with another vendor's capability profile: %zu/%zu\n", vendorPass, vendorPass + vendorFail);
    if (fail == 0 && vendorFail == 0) {
        printf("PASS: %zu/%zu replacement(s) are recognised after being rebuilt the way\n", pass, pass + fail);
        printf("      another GPU's driver would rebuild them.\n");
        printf("      => this folder is portable and will be picked up on NVIDIA/Intel too.\n");
    } else {
        printf("FAIL: %zu of %zu replacement(s) were NOT recognised (%zu/%zu with names),\n",
            fail, pass + fail, namedPass, pass + fail);
        printf("      and %zu of %zu were lost once the capability profile changed.\n",
            vendorFail, vendorPass + vendorFail);
        printf("      Send this text (or a screenshot of this window).\n");
    }
    printf("\nPress Enter to close...");
    (void)getchar();
    return fail == 0 ? 0 : 1;
}
