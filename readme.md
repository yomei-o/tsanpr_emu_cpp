# tsanpr-min

TS-ANPR の C サンプルを、外部ライブラリ依存ゼロの最小構成にしたもの。
同じ実行ファイルを x86 エミュレータ上でも動かせるよう、エミュレータ本体も同梱している。

## 構成

```
tsanpr-min/
├── CMakeLists.txt        ← anpr.exe と x86emu.exe を両方ビルド
├── anpr.exe              ← サンプル（engine/ img/ と同階層に置く必要あり）
├── x86emu.exe            ← x86 エミュレータ
├── src/
│   ├── anpr.c            ← サンプル本体
│   ├── tsanpr.c / .h     ← エンジンの動的ロード（LoadLibrary / dlopen）
│   └── stb_image.h       ← 画像デコード（単一ヘッダ、public domain）
├── emu/
│   ├── src/              ← x86_emu_cpp のソース（本件の追加フック込み）
│   └── LICENSE           ← MIT
├── engine/
│   ├── tsanpr.dll        ← エンジン本体（TS-ANPR v3.1.7M / windows-x86_64）
│   ├── tsanpr-2512M.eon  ← 学習済みモデル（167MB）
│   └── tshelper.exe      ← ライセンス管理
├── img/JP/               ← サンプル画像
└── backup/               ← ライセンス関連レジストリキーの退避（.reg）
```

## ビルド

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

外部ライブラリは不要。両方の実行ファイルがリポジトリ直下に出力される。

## 実行

```sh
anpr.exe              # ネイティブ実行 — 動く
x86emu.exe anpr.exe   # エミュレータ上 — ライセンス判定で止まる
```

`engine/` と `img/` は **exe の位置からの相対**で解決するので、カレントディレクトリはどこでもよい。

ネイティブ実行の結果:

```console
$ ./anpr.exe
img/JP/licensePlate.jpg (outputFormat="text", options="")  => 多摩500さ4649
img/JP/multiple.jpg     (outputFormat="text", options="vm") => 品川302な1234
                                                              品川257め7890
                                                              練馬500く5678
                                                              多摩585ひ9012
                                                              足立460み3456
img/JP/surround.jpg     (outputFormat="text", options="vms") => 品川580こ7861
                                                                帯広230あ235
                                                                京都400そ4720
                                                                神戸552さ27
                                                                函館331ぬ105
                                                                越谷300ち7985
```

## 本家サンプルからの変更点

- libpng / libjpeg → `stb_image.h` に置換（`read_png` + `read_jpeg` の 132 行を
  `read_image` 1 本に集約）。stb は RGB 順で返すため、エンジンが期待する BGR に
  in-place で入れ替えている。
- パス解決を exe 相対に変更（本家は cwd 相対の `../..` 固定）。
- 他言語サンプル・他アーキ用バイナリ・ビルド用の分岐を削除。

## 設定を変える

`src/anpr.c` 内の `// TODO:` コメント箇所。

| 変えたいもの | 場所 | 選択肢 |
|---|---|---|
| 国コード | `main()` | `"JP"` / `"KR"` / `"VN"` — `img/<国コード>/` に画像が要る。`anpr_initialize` は 1 プロセス 1 回のみで実行時切替は不可 |
| 入力方法 | `readLicensePlates()` の `anprFunc` | `readImageFile` / `readEncodedImage` / `readPixelBuffer` |
| 出力形式 | `readLicensePlates()` の `outputFormat` | `text` / `json` / `yaml` / `xml` / `csv` |

認識オプション（第3引数）: `""` 単板 / `"vm"` 複数 / `"vmb"` 二輪含む /
`"vms"` 俯瞰 / `"dms"` 物体検出 / `"dmsr"` 物体＋プレート /
`"dmsri<x1,y1,...>"` 多角形 RoI 内。

---

# エミュレータ上での実行

`x86emu.exe anpr.exe` は最後まで走り、ライセンス判定で止まる。

```console
$ ./x86emu.exe ./anpr.exe
anpr_initialize() failed (error=error: (105) License not installed)
[exit] code 0 after 3489120 instructions
```

**ライセンスゲートより手前は全部動いている**：`tsanpr.dll`（46MB）の実ロードと再配置、
DLL に埋め込まれた OpenCV の CPU baseline チェック、同じく埋め込まれた OpenSSL の
初期化、スレッド・SRWLock・条件変数、COM、HID/SetupAPI のデバイス列挙。
未実装命令にもフォールトにも当たらず exit code 0 で終わる。**推論は一度も走っていない。**

## この環境は4層エミュレーションだった

デバッグ中に判明した重要な前提。この開発機は:

```
QEMU（ARMv8 仮想マシン、WMI が返す機種名 "virt-9.1"）
  └ Windows-on-ARM
      └ MS Prism（x64 → ARM64 のバイナリ変換エミュ）  ← 「ネイティブ」実行はここ
          └ x86emu（本エミュレータ）                    ← anpr.exe をさらに解釈
```

実 CPU は ARMv8（`PROCESSOR_IDENTIFIER = "ARMv8 (64-bit) … QEMU"`, `SystemType = ARM64-based PC`）。
x64 バイナリ（`anpr.exe` / `tsanpr.dll` / `x86emu.exe`）はすべて **MS Prism の x64 エミュ上**で走る。
つまり **「ネイティブ実行」すら Prism という別の x64 エミュレータの上**にある。したがって
「ネイティブ vs x86emu」の差分は実質 **「Prism vs x86emu」** の比較で、ライセンスを通した
基準値 `73e3e418...` は Prism が計算した指紋。x86emu はそれと違う `9ef9aab8...` を出す。

## ライセンスの在処

エンジンはライセンスをレジストリに、Microsoft のキーに偽装して置いている。
`tshelper.exe` をエミュレータで走らせて分かった。

```
HKLM\SOFTWARE\Policies\Microsoft\Windows\Policies
    0 = 7bdc5f968470bb50f14e0605ea2aa0f5 + base64(
        "57 0 8 1 0 2415919104 134217728 0 1788914143611424 0 15 0 0 0 0 0")

HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies
    73e3e41855fba8d949120fe4ac51b3f4 = faa5bd0a56fedfe114d0622157b97d65
```

ディスク上にファイルとしては存在しない。ProgramData も AppData も代替データ
ストリームも空振りするのはそのため。

## 判定の流れ

`x86emu.exe -c ./anpr.exe` で追える。

1. USB ドングルを走査 → 無し
2. レジストリからライセンス記録（値 `0`、245 バイト）を読む → **読めている**
3. WMI とネットワークインターフェースからマシン指紋を計算
4. 指紋から導いた 32 桁の値名を引く → **無い** → `(105)`

エンジンがレジストリから読むのはこの 3 つだけ:

```console
[hook] RegQueryValueEx(0) -> type 1, 245 byte(s)
       "7bdc5f968470bb50f14e0605ea2aa0f5NTcgMCA4IDEgMCAyNDE1OTE5MTA0..."
[hook] RegQueryValueEx(1) -> 2
[hook] RegQueryValueEx(9ef9aab8e492c019a78023874e062cac) -> 2
```

## 指紋の入力はホストと一致している

3 の入力はすべてホストの実物を渡していて、ホストを直接叩いた結果と一致する:

```console
[hook] ExecQuery(SELECT Product,SerialNumber FROM Win32_BaseBoard)
[hook]   -> 0 row(s) from the host                     ← 実機も 0 件（仮想マシン）
[hook] ExecQuery(SELECT Name,ProcessorId FROM Win32_Processor)
[hook]   Name = "virt-9.1" (vt 8)                      ← 実機と一致
[hook]   ProcessorId = "0000000000000000" (vt 8)       ← 実機と一致
[hook] GetIfTable2(level 1) -> 28 interface(s) from the host
[hook] ExecQuery(... win32_NetworkAdapter WHERE GUID='{5C737FB0-...}')
[hook]   DeviceID = "0" (vt 8)                         ← 実機と一致
[hook] ExecQuery(... win32_NetworkAdapter WHERE GUID='{B97D8E86-...}')
[hook]   DeviceID = "11" (vt 8)                        ← 実機と一致
```

それでも導かれる値名は `9ef9aab8e492c019a78023874e062cac` で、記録に埋まっている
`7bdc5f96...` ともホストに実在する `73e3e418...` とも違う。`9ef9aab8...` は
レジストリのどこにも存在しない。

## 潰した候補

WMI ブリッジを切ると値名が `3d67a4d1...` に変わるので、WMI が指紋の入力である
ことは確かめられている。そのうえでホストとの差を見つけては潰した:

| 見つけた差 | 対応 | 値名 |
|---|---|---|
| `GetModuleFileName` がモジュールハンドルを無視 | 見るように | 変化なし |
| パスに `\.\` が残る | `.` と `..` を畳むように | 変化なし |
| OS バージョン 10.0.19045 / 実機 6.2.9200 | ホストへ橋渡し | 変化なし |
| CPUID ベンダー GenuineIntel / 実機 AuthenticAMD | 識別情報だけホストへ | 変化なし |
| プロセッサブランド文字列が空 | ホストへ橋渡し | 変化なし |
| プロセッサ数 1 / 実機 4 | ホストへ橋渡し | 変化なし |
| `GetIfTable2Ex` の level 引数を無視 | 渡すように | 変化なし |
| WMI の数値プロパティを文字列で返していた | VARIANT の型を保つように | 変化なし |
| `StringFromGUID2` | 書式は Windows と同一だった（native と1文字単位で照合） | 変化なし |
| `GetSystemInfo` の `wProcessorLevel`/`Revision` が決め打ち（6/0x3A09、実機 21/0x0001） | ホストへ橋渡し | 変化なし |

いずれもエミュレータ側の実際の不忠実さで、直したこと自体に意味はある。
ただ**指紋は動かない**。観測できる入力はすべてホストと一致しており、
差がどこから来ているかは分かっていない。

### CPU 命令は Prism と一致している（命令エラー説の検証）

「エミュが命令を間違えてハッシュを狂わせている」説を、**同じ実行ファイルを native(Prism) と
x86emu で走らせて出力を突き合わせる差分テスト**で検証した。gcc と MSVC(clang-cl) の両コード生成で:

- 整数 ALU・`rol/ror/shld/shrd/bswap`・128bit 乗算 — 一致
- **MD5 / SHA-1 / SHA-256** — 一致、かつ既知テストベクタとも一致
- **AES-NI・PCLMULQDQ・SSE2/SSSE3(`pshufb`)/SSE4.1・SSE 浮動小数** — 一致
- **CRT の `memcpy`/`memmove`/`memset`/`wmemcpy`**（preimage を組み立てる処理）を多数のサイズ・
  アライメントで — 一致

指紋が使いうる命令はすべて Prism と一致した。唯一見つかった命令バグ（未実装の `HADDPD` 系）は
指紋経路では踏まれない。**命令エラー説は薄い。**

### 値名とデータは暗号的に結合している

x86emu が引く値名（誤り `9ef9aab8`）の照会に、実在する `73e3e418` の**データ `faa5bd0a…` を返しても
`(105)` のまま**。データは指紋に紐づいており、別の値を差し替えても通らない。**エミュが正しい指紋を
計算する以外に道は無い**（＝そもそも値の捏造では突破できない設計＝これは純粋に忠実性の問題）。

### 指紋は上流で逐次ハッシュされている

指紋ルーチン（`tsanpr.dll+0x14B8BB0`）のスタックフレームを実行時にダンプすると、乗るのは
ライセンス記録（`7bdc5f96…` ＋ base64 ペイロード）と**計算済みの指紋 `9ef9aab8`**、レジストリの
サブキー文字列だけ。**機械プロパティ（`virt-9.1`・GUID・DeviceID）は一度も現れない** ＝ 実行時指紋は
この関数の手前でプロパティを集めながら逐次ハッシュされ、ここには 16 バイトの結果だけが渡る。
連結済みの preimage 文字列は存在しないので、単純なメモリダンプでは捕まえられない。関数入口の引数は
`""` と `"none"` を参照していた — **native では実値なのにエミュでは空/none になっているプロパティが
1個あれば、それが逐次ハッシュに食われて指紋がズレる**、という筋（`GetSystemInfo` と同じ
「取りこぼし/決め打ち」パターン）が現時点の最有力。真犯人はこの上流の収集コードにいる。

### ネイティブを gdb で実測した（参照デバッグ）

「ネイティブが計算する正しい指紋」を参照として取るため、gdb でネイティブ実行を直接計測した。
この開発機は **ASLR が効いていて tsanpr.dll のロード base が毎回変わる**うえ、同梱の gdb は
Python 非対応なので base+RVA を実行時算出できない。そこで **tsanpr.dll のコピーの PE ヘッダで
`DYNAMIC_BASE` フラグ(0x0040)を落として ASLR を無効化**し（ロジックもライセンスも触らない・可逆。
計測後に原本へ復元）、固定 base `0x180000000` で計測した。判明したこと:

- **ネイティブの指紋値名 = `73e3e41855fba8d949120fe4ac51b3f4`**（`+0x14B927B` の codepage 変換直前で
  rdx から直読）。レジストリに実在する値そのもので、だからネイティブは通る。エミュは `9ef9aab8…`。
- **ネイティブもエミュも同じコード（`+0x14B927B` など）を通る。** 途中で「ネイティブは指紋経路に
  来ない＝別経路」と見えたのは、ASLR で base がズレた run で誤アドレスにブレークしていた
  アーティファクト。**撤回。両者は同じ指紋経路を通り、値だけ違う。**
- **入力は一致:** ライセンスオブジェクト（`+0x2C87A50`）の DLL パス `C:\prog\…\tsanpr.dll`、
  ユーザパス `C:\Users\…\`、暗号プロバイダ選択 index（両者 `-1`）— native と emu で完全一致。

→ **同じコード・同じ（確認できた）入力なのに、ハッシュ出力だけ違う。**

### ここで詰まっている理由（正直な限界）

2つの壁が重なる:

1. **ハッシュが埋め込み＆多層抽象**。暗号プロバイダのメソッドは no-op スタブ（例 `+0x146F910` は
   `xor eax,eax; ret`）で、フック API も使わない。実ハッシュの供給地点（preimage を食う箇所）を
   静的にも動的にも特定できていない。
2. **ネイティブが MS Prism（x64→ARM64 JIT）上で動く**。gdb のブレークは一部アドレスでしか効かず、
   レジスタは箇所により不正確、HW ウォッチポイントは発火しない。**このマシンではネイティブの
   preimage を確実には抜けない**（値名のように「当たれば正確」な単発読みは取れる）。

観測・制御できる入力はすべて一致、命令も Prism と一致、なのに指紋だけズレる——という状態のまま、
バイト単位の食い違いは未特定。

### これを本当に割るには

- **実機 x86（Prism を挟まない Intel/AMD 機）**で anpr.exe を gdb/x64dbg で普通にデバッグし、
  ネイティブの preimage を抜いてエミュのと diff すれば、ほぼ確実に真犯人（食い違う 1 バイト）が出る。
- または **TS-Solution から指紋アルゴリズム/仕様**を得る。

この 4 層エミュレーション（QEMU-ARM → WoA → Prism → x86emu）の上で、これ以上バイト単位に迫るのは
費用対効果が急落する。実機 x86 が一台あれば一気に片づく、というのが現時点の誠実な結論。

ここから先、値を当てずっぽうに変えて値名を合わせにいくのは、エミュレーションの
忠実さではなくライセンス判定の回避になるので、やらない。

## tshelper をエミュレータで動かす

完走する。ダイアログの中身も読める:

```console
$ ./x86emu.exe -c engine/tshelper.exe
[hook] dialog "" with 8 control(s)
[hook]   control id 1000  Static   "#1"
[hook]   control id 1004  Edit     ""
[hook]   control id 1002  Button   "order"
[hook]   control id 1003  Button   "certification"
[hook]   control id 2     Button   "close"
```

**「30日トライアルを入れる」ボタンは無い。** `order`（要求ファイル生成）、
`certification`（購入済みキーの入力）、`close` だけ。トライアルはボタンで入れる
ものではない。

`--dialog-command 1002` で `order` を押すと、ハードウェア情報の同意ダイアログを
経て、より広い指紋（`MachineGuid`、`ProductId`、`win32_OperatingSystem`、
`Win32_Processor` の Architecture/Manufacturer まで）を集めたあと、
オンライン認証を試みて C++ 例外で止まる。オフライン経路にはまだ届いていない。

## エミュレータに加えた変更

| ファイル | 内容 |
|---|---|
| `hooks_wmi.cpp`（新規） | WMI を COM オブジェクトとして実装。vtable をゲストに合成し、Windows ホストでは実 WMI に橋渡し。プロパティの VARIANT 型も保つ |
| `hooks_registry.cpp`（新規） | レジストリをホストの実レジストリへ橋渡し。既定は読み取りのみ、`--registry-write` で書き込み解禁 |
| `hooks_win32_gui.cpp`（新規） | ヘッドレスなウィンドウシステム。ダイアログテンプレートを解析して控えを実ウィンドウとして作り、`GetDlgItem` が引けるようにする。`MessageBox` は既定ボタンの答えを返す |
| `hooks_winsock.cpp`（新規） | Winsock 一式。序数インポート対応込みで「ネットワークは無い」と正直に答える |
| `hooks_win32d.cpp`（新規） | InitOnce、プロセッサトポロジ、シェルフォルダ、パス文字列、HID/SetupAPI、証明書ストア、CryptoAPI の鍵操作、COM、`GetIfTable2` のホスト橋渡し |
| `cpu.cpp` | CPUID に SSE3（プリビルドの x86-64 OpenCV はこのビットが無いと起動を拒否する）。識別情報とブランド文字列はホストの実値 |
| `sse.cpp` | **SSE3 の水平加減算 `HADDPD/HADDPS`・`HSUBPD/HSUBPS`・`ADDSUBPD/ADDSUBPS`（0F 7C/7D/D0）を実装**。CPUID で SSE3 を広告しているのに未実装で、`unsupported opcode 0x7C` で落ちていた（差分テストで発見） |
| `hooks_win32.cpp` | `GetModuleFileName` がモジュールハンドルを見るように。パスを正規化。プロセッサ数をホストの実数に。**`GetSystemInfo` の `wProcessorLevel`/`wProcessorRevision`/`dwProcessorType`/アクティブマスクをホストの実値に**（従来は level=6・rev=0x3A09 の決め打ちで、実機の level=21・rev=0x0001 と食い違っていた） |
| `modules.cpp` | **`GetModuleFileName` が返すパスの case を保存**。従来は `load_library` がパス全体を小文字化して格納し、`GetModuleFileName` が `c:\…`（小文字ドライブレター）を返していた。Windows は case 保存で `C:\…` を返す。照合キーだけ小文字化し、パスは元の綴りを保つように修正（`tsanpr.dll` のパスは指紋には効いていなかったが、実機との差ではあった） |
| `hooks_win32b.cpp` | `GetVersionEx` をホストの値に |
| 計測 | `--trace-calls` に CreateFile のパス、レジストリのキー名/値名/内容、`sprintf` の生成文字列、ダイアログの構造を出す |

追加オプション:

```
      --registry-write     ゲストにホストのレジストリを書かせる
      --dialog-command ID  ダイアログを開いた後 WM_COMMAND ID を送る
```

**未検証**: CPUID・`GetVersionEx`・プロセッサ数の変更は他のゲストにも影響するが、
本家の `tests/` を持っていないため回帰テストにかけていない。CPython や cl.exe の
ケースが通るかは未確認。

## ライセンス

- サンプルコード: MIT (TS-Solution Corp.)
- `stb_image.h`: public domain / MIT
- `emu/`: MIT — github.com/yomei-o/x86_emu_cpp
- **エンジン (`tsanpr.dll`, `.eon`): 商用製品**。無料期間は 1 システムあたり 30 日。
  継続利用には TS-Solution からのライセンス購入が必要。再配布不可。
