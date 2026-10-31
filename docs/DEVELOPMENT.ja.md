# Playload Handheld 開発者ガイド

[English](DEVELOPMENT.md)

Playload Handheld の作り、動作の仕様、性能チューニングの内容をまとめます。利用者向けの説明は [README](../README.ja.md)、計測値は [BENCHMARKS.ja.md](BENCHMARKS.ja.md) にあります。

## 目標

1. Playload のゲーム URL を、64bit ARM の携帯機でオフラインで動かす。ハードウェアが許す限り 60fps で。
2. ブラウザ版とまったく同じ画面・同じ音にする。
3. 本家のエンジンをフォークせずに追従する。本家の `engine.js`・`sound.js`・`glyphs.js`・`crt.js` を公開されたまま使い、アプリ内からも更新できるようにする。

## アーキテクチャ

```
playload.aarch64（C、1プロセス）                             node（子プロセス、任意）
┌──────────────────────────────────────────────────────┐   ┌──────────────────────────────┐
│ SDL2：ウィンドウ、コントローラ、音声デバイス            │   │ node-host.mjs                │
│ video.c：OpenGL ES の表示（通常／CRT シェーダー）        │   │  └─ 本家 engine.js、         │
│ ay.c：AY-3-8910 の合成（音声スレッド）                  │   │     sound.js、glyphs.js      │
│ raster：160×144 のラスタライザ                          │◀─▶│    （render() を差し替え）   │
│ inflate.c：Playload の URL の展開                       │   │  ゲーム：tick() を 60Hz      │
│ QuickJS-ng：メニュー、エラー画面、更新の自己テスト、     │   └──────────────────────────────┘
│            RUNTIME = QUICKJS のときのゲーム              │      パイプ：依頼は fd 3、
│   └─ host.js ＋ patch.js ＋ 同じ本家ファイル              │              返信は fd 4
└──────────────────────────────────────────────────────┘
```

計測から得た設計の原則は、**ゲームのロジックは JavaScript（本家のコード）で、1画素・1サンプル単位の処理はネイティブで**です。これらの SoC では、JavaScript はゲームのロジックには十分速いものの、毎フレーム 23,040 画素を塗ることや、毎秒 48,000 サンプルをオーバーサンプリングして合成することには向きません。

### 1フレームの流れ

- **QuickJS で動かすとき**：入力 → `engine.step(tick)` → `engine.pixels`（差し替えた `render()` が画面のマスとスプライトをネイティブのラスタライザに渡す）→ 音を音声スレッドのキューへ → GPU で表示。すべて1スレッドで順番に行います。
- **Node.js で動かすとき**：プレイヤーは常に1 tick 先の計算を依頼しておきます。フレーム *n* を表示して垂直同期を待つ間に、Node.js が tick *n+1* を計算します。プレイヤーは返信（画面の内容と音）を受け取ってラスタライズし、その時点のボタンで次の tick を依頼します。JavaScript と GPU・垂直同期が重なる代わりに、入力の反映が1フレーム遅れます。

tick は 60Hz の固定ステップで進みます。5 tick 以上遅れたときは追いつこうとせず、その分の時間を捨てます（本家のブラウザ版と同じ）。

## ソースの構成

| パス | 内容 |
|---|---|
| `src/player.c` | 本体：オプション、ゲーム一覧、設定、QuickJS のホスト、セッション、入力、画面なしモード、エンジンの更新、Arcade（一覧・保存・プレビュー） |
| `src/noderun.c` | Node.js の子プロセス：起動、依頼／返信のやり取り、タイムアウト |
| `src/video.c` | OpenGL ES の表示：通常表示、CRT シェーダー（本家 `crt.js` から作成）、診断機能 |
| `src/ay.c` | 本家 `sound.js` から移植した AY-3-8910 のモデル |
| `src/inflate.c` | Playload の URL 用の raw DEFLATE デコーダ（puff 方式） |
| `js/playload/` | 本家の `engine.js`・`sound.js`・`glyphs.js`・`crt.js`（無改変）。`VERSION` に元のコミットを記録 |
| `js/patch.js` | QuickJS 用：読み込み時の差し替え、`Intl.Segmenter` の代替、各種キャッシュ、`console` |
| `js/host.js` | QuickJS 用：プレイヤーと本家エンジンをつなぐ部分 |
| `js/menu.js` | メニュー。それ自体が Playload のゲーム（メイン画面、Arcade の一覧と詳細画面） |
| `js/node-host.mjs` | Node.js 用：ゲームのホスト、読み込み時の差し替え、画面と音の送信 |
| `port/` | `Playload.sh`（PortMaster の起動スクリプト）と同梱ゲーム |
| `vendor/` | QuickJS-ng（amalgamation）、SDL 2 のヘッダ、リンク用の `libSDL2` |
| `tools/` | `embed.py`（JS をバイナリに埋め込む）、`package.sh`（配布物の作成）、`dev/`（計測用ボット、参照レンダラ） |

## 仕様

### ゲームファイル

- `games/*.url`：Playload の URL を含むテキストなら何でも可。`#v1.`（旧形式の `#1.` も可）より後ろを取り出し、base64url をデコードして raw DEFLATE を展開します（ソースは最大 1MiB）。`#` を含まないファイルは JavaScript のソース（`.js`）として扱います。
- ゲームが1本なら直接起動し、複数ならメニューを出します。タイトルはゲームの `// @title` 行、なければファイル名です。

### 表示

- Playload の 160×144 の画面と8色のパレットを、画面に収まる最大の整数倍で拡大して中央に表示します（640×480 なら3倍）。`PLAYLOAD_SCALE=fit` で画面いっぱいに拡大します。
- 表示には SDL のレンダラではなく OpenGL ES を直接使い、通常表示と CRT 表示を同じ仕組みで描きます。
- **CRT 表示**：頂点・フラグメントシェーダーは、実行時に本家の `crt.js` から取り出します（テンプレートリテラルの `vertexSource`／`fragmentSource`）。そのため、エンジンの更新に追従します。GL のコンテキストが ES 3 なら、シェーダーの `fetch()` を差し替えて、パレット色を入れた sRGB8 テクスチャを読むようにします。それ以外の環境では、本家どおりのパレット番号方式で描きます。`crt.js` の解析やコンパイルに失敗したときは、直前のシェーダーを使い続けます。

### 入力

Playload の6つのボタンをマスクに変換します（上 1、下 2、左 4、右 8、A 16、B 32）。コントローラの十字キーと左スティック（しきい値 16000）、A／B（`BUTTONS = XBOX` なら入れ替え）、キーボード（矢印／WASD、Z／J、X／K）のどれでも入力できます。SELECT＋START でメニューに戻る（メニューでは終了）、SELECT＋R1 で CRT、SELECT＋L1 で性能表示を切り替えます。いずれも押した瞬間に1回だけ反応します。

### 音

`play()` の楽譜は本家の `compileScore`（JavaScript）で解釈し、`[op, a, b]` の3つ組に平坦化して音声スレッドのキューに入れます。音声スレッドでは `ay.c` が再生します。矩形波3チャンネル、共有の17ビットノイズとエンベロープ、8倍オーバーサンプリング、257タップの窓関数付き sinc フィルタで、`sound.js` とまったく同じ計算です。出力は 48kHz（機体が許可した値）で、ステレオの両チャンネルに同じ音を出します。新しい `sound.js` で増えた未知の命令は読み飛ばします。

### 設定（`playload.cfg`）

| キー | 値 | 初期値 |
|---|---|---|
| `buttons` | `nintendo`、`xbox` | `nintendo` |
| `crt` | `on`、`off` | `off` |
| `fps` | `on`、`off` | `off` |
| `runtime` | `node`、`quickjs` | `node/node` があれば `node` |
| `engine` | `builtin` または `engine/` 内のフォルダ名 | `builtin` |
| `last` | 最後に遊んだゲームのファイル名 | 先頭のゲーム |

環境変数で上書きできます：`PLAYLOAD_BUTTONS`、`PLAYLOAD_CRT`、`PLAYLOAD_FPS`、`PLAYLOAD_RUNTIME`、`PLAYLOAD_SCALE`。

### メニュー

メニューは、それ自体が QuickJS で動く Playload のゲームです（`js/menu.js`。エンジンのファイルと同じように本体に埋め込みます）。プレイヤーとは `__native` を通じてやり取りします：`launch(i)`、`layout(v)`、`crt(v)`、`runtime(v)`、`engine(name)`、`update()`、Arcade 用に `arcade(cachedOnly)`、`payloads()`、`save(name, url)`、`preview(url)`、`state(s)`。ゲームは1本ごとに新しい JavaScript 実行環境で動くので、前のゲームの状態は残りません。

**Arcade**
- **一覧の取得**：本家の `data/arcade.json`（タイトル、作者、作者の URL、ゲームの URL、スクリーンショットのパス）を使います。更新機能と同じ `curl` で `raw.githubusercontent.com/kitao/playload/main/` から取得し、オフラインでも使えるように `arcade/arcade.json` に保存します。HTML の解析はしません。
- **保存**：完全な URL を `games/<スクリーンショット名>.url` に書きます（相対 URL には `https://kitao.dev` を付けます）。同じ名前のファイルがあっても上書きしません。`games/` のどれかのファイルが `url` か `originalUrl` と同じ `#v1.` のデータを持っていれば、保存済みとみなします。
- **プレビュー**：2つ目の無音の QuickJS 実行環境で、メニューの1 tick ごとに入力なしで1 tick ずつゲームを動かします。メニューの画面を描いた後、(40, 24) に半分の大きさで重ねます（2×2 の中で最初の黒以外の画素）。1 tick が 40ms を超えるゲームは、メニューの反応を保つため、そのフレームで止めます。
- **描けない文字**：フォントはかなを含みますが、漢字はわずかです。形が空の文字を含むタイトルや作者名は、スクリーンショット名や X のアカウント名で表示します（`drawable()` が `pcg[c].pat` を調べます）。
- **ゲームから戻るとき**：Arcade から保存済みのゲームを遊ぶと、`state()` に `arcade <番号>` を記録します。ゲームの後は、保存してある一覧からそのゲームの画面を開き直します。

### JS エンジン（RUNTIME）

- **Node.js**（入っていれば初期値）：`node/node` を起動します。`LD_LIBRARY_PATH` は `node/lib` を先頭に、その後ろに Ports の起動環境のパスを続けます。`LD_PRELOAD` は空にします（plumOS が GPU ドライバなどを先読みさせていますが、Node.js には不要なため）。
- **QuickJS-ng**（内蔵）：メニュー、エラー画面、更新の自己テストに使います。設定で選んだときのゲームにも使い、Node.js が起動に失敗したとき（ライブラリ不足、起動直後のクラッシュなど）も、そのゲームは自動で QuickJS で動かします。

### Node.js とのやり取り

依頼は子プロセスの fd 3 に送ります：`'F' <マスク>`（1 tick 実行）、`'Q'`（終了）。返信は fd 4 に、種類の1バイト、リトルエンディアン u32 の長さ、本体の順で届きます。

| 返信 | 本体 |
|---|---|
| `R` | 画面の内容：360マス×（パターン8行、インク、ペーパー）、u16 のパターン数とパターン8行ずつ、u32 のスプライト数、続いて列ごとに `x i16[]`、`y i16[]`、`pattern u16[]`、`col u8[]`（画面にかかるスプライトのみ）。続いて音、最後に末尾データ |
| `F` | パレット番号 23,040個（描画の差し替えができなかったときの本家描画）。続いて音、最後に末尾データ |
| `E` | UTF-8 のエラーメッセージ（プレイヤーがエラー画面に表示） |

音：u16 の個数、続いて1音ごとに u8 チャンネル、u8 固定音量フラグ、u32 のイベント数、f64 の `[op, a, b]` の並び。末尾データ：この tick の後に待つフレーム数（u8）とノイズの有無（u8）。`R` でノイズありのときは、続けて画面に XOR する上限超過のノイズ 23,040 バイト（Playload 0.9.4 以降、下記参照）。最初の返信（ゲームのトップレベルのコードと最初の tick の後）は、依頼なしで届きます。返信を待つ時間は、開始時 15秒、各 tick 3秒で、超えるとゲームが応答しないとみなします。標準出力と標準エラーは `log.txt` に入ります。

### エンジンの版とアプリ内更新

- `builtin` はバイナリに埋め込まれています（Node.js 用に `node/builtin/` にもコピー）。ダウンロードした版は `engine/<タグ>/` に置かれます。
- **ENGINE → A**：`api.github.com/repos/kitao/playload/tags` から最新のタグを取得し（plumOS の `curl` を、環境変数を整理したうえで使用）、そのコミットの4ファイルを `engine/.new` にダウンロードします（1回の `curl` で、1つの接続を使い回します）。自己テストに合格したら `engine/<タグ>` に名前を変えて選択します。すでに揃っている版があれば、それを再利用します。
- **自己テスト**：API の総合テスト用ゲーム（Playload の全関数・全変数を使い、文字が実際に描かれることも確認）と、`games/` のゲーム最大6本（同梱ゲームを優先）を、決まった入力で QuickJS で無音で実行します。1本あたり 300 tick か、最初の 30 tick の後 1.5 秒のどちらか早い方までです。例外が出ず、全フレームがネイティブのラスタライザを通った場合だけ合格です。実行中は、メニューの右下に `DOWNLOAD`、`TEST n/m` と表示します。
  以前は全ゲームを最後まで動かしていたため、数分かかることがありました（RK3566 で13本なら89秒、うち rubble. だけで43秒）。上限を設けた今は、自己テストが7秒、更新全体でも約9秒です。
- この方法で更新できるのは JavaScript の部分だけです。本家の音の合成（`ay.c`）や URL の形式（`inflate.c`）が変わったときは、プレイヤー自体の再ビルドが必要です。

## 読み込み時の差し替え

本家のファイルはディスク上では一切書き換えず、読み込むときに調整します。必須でない差し替えは、目印の文字列が見つからなければ行いません。本家のファイルが変わっても、遅くなるだけで正しく動きます。

| 差し替え | 対象 | 必須か | 理由 |
|---|---|---|---|
| `render()` をネイティブのラスタライザ向けに（画面のマス＋スプライト） | 両方 | QuickJS：必須。Node.js：任意（できなければ本家の描画） | 本家は全画素を JS で塗る。RK3566 で QuickJS は 22〜24ms、Node.js は1万スプライトで 23ms。0.9.3 から `pcg[chr].pat` は書き換え回数を数える Proxy になったため、エンジン内部の `patterns`（WeakMap）から生のパターンを読む（Proxy 経由だと QuickJS で BRR 3KB が約3倍遅くなった） |
| `render()` が、スプライトや PCG の上限を超えたフレームでノイズ（`glitches`）も渡し、ネイティブ側で画面に XOR する | 両方 | 任意（0.9.4 より前にはない） | 0.9.4 から、上限を超えるとノイズを表示する。`step()` の戻り値（待つフレーム数。超えた上限1つにつき1）は、ブラウザ版と同じようにセッション中に待つ。待つ間も音は鳴り続け、その後 60Hz の予定に追いつく |
| `characterKey()` の結果を記憶（例外になる入力は記憶しない） | 両方 | 任意 | `spr()`／`put()` のたびに文字列の正規化・照合・区切りを行うため |
| `Intl.Segmenter` の結果を文字列ごとにキャッシュ | 両方 | — | QuickJS には `Intl` がない（近似で代替）。Node.js では ICU の区切り処理が `put()` の多いゲームの大半を占めた |
| `String.prototype.normalize` をキャッシュ | QuickJS | — | 毎 tick 同じ文字列のため |
| `colorPair` の中身の配列を取り出せるように | Node.js | 任意 | 本家の `Proxy` 経由の色の読み取りが、360マスで毎フレーム約 8ms |
| 文字の情報を文字列ごとにキャッシュ | Node.js | — | 下記「V8 の最適化の取り消し」参照 |
| 最初の tick の前に `-0` 座標のスプライトを作る | Node.js | — | 下記「V8 の最適化の取り消し」参照 |
| `console` をログへ | QuickJS | — | ブラウザ向けのゲームが `console.log` を呼ぶことがあるため |

## 性能チューニングの記録

数値はすべて実機での計測です。詳細は [BENCHMARKS.ja.md](BENCHMARKS.ja.md) を参照してください。

1. **JS エンジンの選択。** 本家をそのまま QuickJS で動かすと、RK3566 で 1フレーム 26〜32ms（描画だけで 22〜24ms）、AY 音源は CPU 885% でした。Node.js（V8）は速いものの、ときどき 70〜360ms 止まりました。そこで最初の版は描画と音を C に移し、QuickJS を使う構成にしました（1フレーム 1.2〜3.4ms）。
2. **本家のファイルを無改変で使う。** 本家の更新に追従するため、手で書き直したエンジンをやめ、本家のファイル＋読み込み時の差し替えに切り替えました（`characterKey`・区切り・正規化のキャッシュ後で 1フレーム 3.2〜4.9ms）。
3. **Mali での CRT。** 本家のシェーダーは、1画素あたり31回の読み取りのたびに、比較の連鎖でパレット色を探します。Mali-G52 で GPU 時間 21.9ms。GPU に sRGB8 テクスチャを渡す方式（GPU がリニアに変換する。`crt.js` と同じ変換式）にして 7.0ms、画素の差は最大 2/255 です。
4. **重いゲームのための Node.js。** 毎フレーム約1万個のスプライトを描くゲーム（AFTER LOAD HD）は、QuickJS で 6〜10fps でした。Node.js ではゲームの処理が 2.6ms でしたが、数秒たつと速い状態から約10fps に落ちました。V8 のトレースで原因が分かりました。
   - **オブジェクトの形の変化による最適化の取り消し。** 本家は文字の情報を初めて使うときに作ります。新しい文字が初めて出るたびに文字の表の形が変わり、V8 はそれに触れる最適化済みのコードを捨て、やがて最適化をあきらめていました。描画では文字の情報を文字列で覚えておき、ループの中では文字の表に触らないようにしました。
   - **`-0` 座標。** ゲームの途中で初めて `x = -0`（`Math.trunc(-0.3)` の結果）のスプライトができると、スプライトのフィールドの表現が変わり、TurboFan の最適化が中断されました。最初の tick の前にそのようなスプライトを1つ作り、最初から形を決めておくようにしました。
   - 結果：描画データの作成が 36ms → 2.6ms、AFTER LOAD HD は 52ms（QuickJS）→ 7.1ms になりました。
5. **`put()` の多いゲームでの ICU。** 10SEC RACE は毎フレーム `put()` を約250回呼び、JIT の効かないネイティブの ICU による区切り処理が大半を占めていました。区切りと `characterKey` のキャッシュで 19.6ms → 7.7ms になりました。
6. **並行処理（パイプライン）。** rubble. は毎フレーム約12ms の JavaScript が必要です。GPU の処理や垂直同期と順番に行うと、ときどき垂直同期に間に合わず、遅れを取り戻す tick がさらに次のフレームを遅らせて、60 TPS のまま約20fps になっていました。表示の前に次の tick を依頼することで両者が重なり、60fps になりました。
7. **まず測る。** これらの問題の多くは Mac ではまったく見えませんでした（V8 の最適化の取り消し、ICU のコスト、GPU のコスト）。下の各種ツールは、そのために用意したものです。

## 機種ごとの情報

| plumOS の種類 | SoC／GPU | Ports フォルダ | plumOS のルート | 注意点 |
|---|---|---|---|---|
| Bubble | RK3566／Mali-G52（libmali、GLES 3.2） | `/storage/user/Roms/PORTS` | `/storage/plumos` | ガバナーは `plumos-cpu-control` で切り替え |
| Pixel2 | RK3326／Mali-G31（Mesa Panfrost、GLES 3.1） | `/roms/PORTS` | `/storage` | 縦長 480×640 のパネル。起動スクリプトが SDL／GL の回転ライブラリ（270°）を先読みさせるので、プレイヤーは普段どおり 640×480 で描く |
| V90S | A133P／PowerVR GE8300（GLES 3.2） | `/mnt/plumos/roms/ports` | `/mnt/plumos` | SDL の映像ドライバは `mali`。Debian 12 ベース。SSH を閉じると裏のプロセスが止まり、`/tmp` は接続ごとに別 |

`Playload.sh` は、起動中だけ CPU ガバナーを `performance` にします（`plumos-cpu-control` があればそれで、なければ sysfs で直接）。終了時には元に戻します。フロントエンドの停止と復帰は、plumOS の Ports 起動スクリプトが行います。

## ビルド

必要なもの：macOS または Linux、[zig](https://ziglang.org/) 0.15.x（クロスコンパイラ）、Python 3。

```bash
./build.sh            # build/playload.aarch64（実機用）と build/playload-mac（開発用、画面なし）
./build.sh aarch64    # 実機用のみ
tools/package.sh      # dist/playload-handheld-<バージョン>.zip
```

実機用バイナリは `aarch64-linux-gnu.2.36` 向けで、SDL 2 は動的にリンクします（機体側のものを使用）。必要なのは `libc`、`libm`、`libSDL2-2.0.so.0` だけです。`tools/package.sh` は Node.js と Debian のライブラリを、SHA-256 を固定して取得します。

## テストと診断

| コマンド／環境変数 | 用途 |
|---|---|
| `--headless --ticks N <ゲーム>` | 画面なしで実行し、tick ごとの時間を表示（QuickJS） |
| `--headless --runtime node` | 同じことを Node.js で（`PLAYLOAD_NODE_ROOT` で Node.js の場所を指定、`PLAYLOAD_NO_INPUT=1` で入力なし） |
| `--seed N --inject tools/dev/bot-brr.js` | 乱数を固定し、ボット（Bit-Rate Rush 3KB 用）で決まった展開を再現 |
| `--dump-at T --dump f.ppm`、`--dump-audio f.f32` | 比較用の画面・音の書き出し |
| `tools/dev/refdump.mjs` | 本家のエンジンそのものを QuickJS で同じ条件で動かして画面を書き出す。プレイヤーの画面と1バイト単位で一致すること |
| `--selftest DIR`、`--update`、`--menu-dump f.ppm` | 更新の自己テスト、更新、メニュー画面の画像化 |
| `--version` | バージョン表示（SDL のライブラリが読めるパスで実行） |
| `PLAYLOAD_STATS=1` | 300 tick ごとに、待ち時間、表示回数、20ms を超えたフレーム数、最長のフレームを出力 |
| `PLAYLOAD_AUTOPLAY=1` | 実際のセッションで自動操作（60フレームごとに A を10フレーム、十字キーを順番に） |
| `PLAYLOAD_PROFILE=1` | Node.js：tick／描画／送信の時間の内訳 |
| `PLAYLOAD_NODE_FLAGS="--trace-deopt …"`、`NODE_OPTIONS=--cpu-prof` | V8 のトレースと CPU プロファイル |
| `PLAYLOAD_GPU_TIMING=1`、`PLAYLOAD_SHOT=f.ppm PLAYLOAD_SHOT_AT=N` | フレームごとの GPU 時間（診断専用）、画面の保存 |

リリース時の確認：乱数を固定したボットの実行で、複数の時点の画面が本家エンジンと一致すること（QuickJS と Node.js の両方）、音が `sound.js` の AY と1サンプル単位で一致すること、同梱ゲームがすべて動くこと、最新の Playload のタグで自己テストに合格すること。

## 組み込みの Playload を更新するとき

1. 新しいタグの `src/engine/engine.js`・`sound.js`・`glyphs.js`、`src/app/crt.js` を `js/playload/` にコピーし、`js/playload/VERSION` を更新する。
2. 差し替えの目印が残っているか確認する（`function render() {` と、そこで渡す `patterns`・`glitches`・`spriteOverflow`・`pcgOverflow` の名前、`const characterKey = (chr) => {`、`const pair = new Proxy(values, {`、シェーダーのテンプレートリテラル、`vec3 fetch(vec2 pixel) {`）。
3. `sound.js` の AY のコードと `crt.js` のシェーダーを前の版と比べ、変更があれば `src/ay.c`／`src/video.c` に反映する。
4. 上記の確認を行い、`tools/package.sh` を実行する。

## 既知の制限

- Node.js で動かすときは、入力の反映が1フレーム遅れ（並行処理のため）、ゲーム開始から数秒は V8 のコンパイルによる短い引っかかりがあります。
- QuickJS では、書記素の区切りを近似しています（基本文字＋結合文字、肌の色、ZWJ のつながり）。
- CRT 表示は、Mali-G31（RK3326）と PowerVR GE8300（A133P）では 60fps に届きません。
- 動作確認は plumOS のみです。
