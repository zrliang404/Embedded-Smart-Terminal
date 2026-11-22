#include "ime.h"
#include "pinyin_dict.h"

#include <string.h>

/* ============================================================
 * 拼音输入法：音节表 + 词库 + 单字兜底
 * ============================================================ */

/* 全部合法拼音音节（按长度分组便于最长匹配） */
static const char *s_syl[] = {
  "a","ai","an","ang","ao","ba","bai","ban","bang","bao","bei","ben","beng","bi","bian","biao","bie","bin","bing","bo","bu",
  "ca","cai","can","cang","cao","ce","cen","ceng","cha","chai","chan","chang","chao","che","chen","cheng","chi","chong","chou","chu","chuai","chuan","chuang","chui","chun","chuo","ci","cong","cou","cu","cuan","cui","cun","cuo",
  "da","dai","dan","dang","dao","de","dei","deng","di","dia","dian","diao","die","ding","diu","dong","dou","du","duan","dui","dun","duo",
  "e","en","eng","er","fa","fan","fang","fei","fen","feng","fo","fou","fu",
  "ga","gai","gan","gang","gao","ge","gei","gen","geng","gong","gou","gu","gua","guai","guan","guang","gui","gun","guo",
  "ha","hai","han","hang","hao","he","hei","hen","heng","hong","hou","hu","hua","huai","huan","huang","hui","hun","huo",
  "ji","jia","jian","jiang","jiao","jie","jin","jing","jiong","jiu","ju","juan","jue","jun",
  "ka","kai","kan","kang","kao","ke","ken","keng","kong","kou","ku","kua","kuai","kuan","kuang","kui","kun","kuo",
  "la","lai","lan","lang","lao","le","lei","leng","li","lia","lian","liang","liao","lie","lin","ling","liu","long","lou","lu","luan","lue","lun","luo",
  "ma","mai","man","mang","mao","me","mei","men","meng","mi","mian","miao","mie","min","ming","miu","mo","mou","mu",
  "na","nai","nan","nang","nao","ne","nei","nen","neng","ni","nian","niang","niao","nie","nin","ning","niu","nong","nu","nuan","nue","nuo",
  "o","ou","pa","pai","pan","pang","pao","pei","pen","peng","pi","pian","piao","pie","pin","ping","po","pou","pu",
  "qi","qia","qian","qiang","qiao","qie","qin","qing","qiong","qiu","qu","quan","que","qun",
  "ran","rang","rao","re","ren","reng","ri","rong","rou","ru","ruan","rui","run","ruo",
  "sa","sai","san","sang","sao","se","sen","seng","sha","shai","shan","shang","shao","she","shen","sheng","shi","shou","shu","shua","shuai","shuan","shuang","shui","shun","shuo","si","song","sou","su","suan","sui","sun","suo",
  "ta","tai","tan","tang","tao","te","teng","ti","tian","tiao","tie","ting","tong","tou","tu","tuan","tui","tun","tuo",
  "wa","wai","wan","wang","wei","wen","weng","wo","wu",
  "xi","xia","xian","xiang","xiao","xie","xin","xing","xiong","xiu","xu","xuan","xue","xun",
  "ya","yan","yang","yao","ye","yi","yin","ying","yo","yong","you","yu","yuan","yue","yun",
  "za","zai","zan","zang","zao","ze","zei","zen","zeng","zha","zhai","zhan","zhang","zhao","zhe","zhen","zheng","zhi","zhong","zhou","zhu","zhua","zhuai","zhuan","zhuang","zhui","zhun","zhuo","zi","zong","zou","zu","zuan","zui","zun","zuo"
};
#define SYL_N  (sizeof(s_syl)/sizeof(s_syl[0]))

/* 词库：拼音（连写）→ 中文词 */
struct Word { const char *py; const char *cn; };

static const Word s_words[] = {
  /* 城市（拼音 → 中文） */
  {"beijing","北京"},{"shanghai","上海"},{"guangzhou","广州"},{"shenzhen","深圳"},
  {"hangzhou","杭州"},{"chengdu","成都"},{"wuhan","武汉"},{"xian","西安"},
  {"nanjing","南京"},{"chongqing","重庆"},{"tianjin","天津"},{"suzhou","苏州"},
  {"changsha","长沙"},{"zhengzhou","郑州"},{"qingdao","青岛"},{"dalian","大连"},
  {"xiamen","厦门"},{"kunming","昆明"},{"hefei","合肥"},{"fuzhou","福州"},
  {"shenyang","沈阳"},{"changchun","长春"},{"haerbin","哈尔滨"},{"jinan","济南"},
  {"shijiazhuang","石家庄"},{"taiyuan","太原"},{"nanchang","南昌"},{"nanning","南宁"},
  {"guiyang","贵阳"},{"haikou","海口"},{"lanzhou","兰州"},{"xining","西宁"},
  {"yinchuan","银川"},{"wulumuqi","乌鲁木齐"},{"huhehaote","呼和浩特"},{"lasa","拉萨"},
  {"wuxi","无锡"},{"ningbo","宁波"},{"wenzhou","温州"},{"dongguan","东莞"},
  {"foshan","佛山"},{"zhuhai","珠海"},{"zhongshan","中山"},{"huizhou","惠州"},
  {"yantai","烟台"},{"weifang","潍坊"},{"luoyang","洛阳"},{"xiangyang","襄阳"},
  {"yichang","宜昌"},{"guilin","桂林"},{"sanya","三亚"},{"heilongjiang","黑龙江"}
};
#define WORD_N  (sizeof(s_words)/sizeof(s_words[0]))

/* 某音节对应的候选字串（UTF-8，每 3 字节一个字） */
static const char *syl_chars(const String &syl)
{
    for (uint16_t i = 0; i < SYLCHARS_N; i++)
        if (syl == s_sylChars[i].py) return s_sylChars[i].cn;

    return "";
}

static String ime_lookup_cn(const String &py_in, int page)
{
    String py = py_in;
    py.toLowerCase();
    py.replace(" ", "");

    if (py.length() == 0) return String("");

    /* 1) 整词匹配（只在第 1 页） */
    if (page <= 1)
    {
        for (uint16_t i = 0; i < WORD_N; i++)
            if (py == s_words[i].py) return String(s_words[i].cn);
    }

    /* 2) 按音节切分 */
    String syls[10];
    int ns = 0;
    String rest = py;

    while (rest.length() && ns < 10)
    {
        int hit = 0;

        for (int n = 6; n >= 1; n--)
        {
            if (rest.length() < (unsigned)n) continue;

            String s = rest.substring(0, n);

            for (uint16_t i = 0; i < SYL_N; i++)
                if (s == s_syl[i]) { hit = n; break; }

            if (hit) break;
        }

        if (!hit)
        {
            /* 前缀兜底：比如 "n" -> 拿/你/年... */
            String pre = "";
            int cnt = 0, skip = (page - 1) * 5, seen = 0;

            for (uint16_t i = 0; i < SYLCHARS_N && cnt < 5; i++)
            {
                if (strncmp(s_sylChars[i].py, py.c_str(), py.length()) == 0)
                {
                    if (seen++ < skip) continue;
                    if (cnt) pre += "|";
                    pre += String(s_sylChars[i].cn).substring(0, 3);
                    cnt++;
                }
            }

            return pre;
        }

        syls[ns++] = rest.substring(0, hit);
        rest.remove(0, hit);
    }

    if (rest.length()) return String("");

    /* 3) 第一个音节出候选，其余音节取首字 */
    String first = syl_chars(syls[0]);
    if (first.length() == 0) return String("");

    String tail = "";

    for (int i = 1; i < ns; i++)
    {
        String c = syl_chars(syls[i]);
        if (c.length() == 0) return String("");
        tail += c.substring(0, 3);
    }

    String out = "";
    int total = first.length() / 3;
    int start = (page - 1) * 5;
    int cnt   = 0;

    for (int i = start; i < total && cnt < 5; i++, cnt++)
    {
        if (cnt) out += "|";
        out += first.substring(i * 3, i * 3 + 3);
        out += tail;
    }

    return out;
}

/* ------------------------------------------------------------
 * 对外接口
 *
 * 第 1 页的第一个候选始终是“输入的原始字母组合”，
 * 这样当拼音对不上汉字时（如 hello、deepseek），
 * 也能直接从候选里把字母插进输入框。
 * ------------------------------------------------------------ */
String ime_lookup(const String &py_in, int page)
{
    String raw = py_in;
    raw.toLowerCase();
    raw.replace(" ", "");

    if (raw.length() == 0) return String("");

    String cn = ime_lookup_cn(py_in, page);

    if (page > 1) return cn;              /* 翻页不重复显示原始串 */

    if (cn.length() == 0) return raw;     /* 没有汉字：只返回字母本身 */

    return raw + "|" + cn;                /* 字母 + 汉字候选 */
}
