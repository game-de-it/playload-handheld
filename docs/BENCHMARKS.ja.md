# Playload Handheld ベンチマーク記録

[English](BENCHMARKS.md)

計測期間：2026-10-22 〜 2026-10-24
対象：Playload Handheld（このリポジトリ）、Playload エンジン v0.9.0（組み込み）／v0.9.1、Node.js v24.22.1（linux-arm64 公式版）、QuickJS-ng 0.17.0

## 機種

| 機種 | SoC | CPU | GPU / ドライバ | 画面 | plumOS |
|---|---|---|---|---|---|
| plumOS-Bubble | Rockchip RK3566 | Cortex-A55 ×4、最大 1.99GHz | Mali-G52、ARM 純正（libmali）、OpenGL ES 3.2 | 640×480 | Bubble 版 |
| GameKiddy GKD Pixel2 | Rockchip RK3326 | Cortex-A35 ×4、最大 1.51GHz | Mali-G31、Mesa Panfrost 22.3.6、OpenGL ES 3.1 | 480×640（縦長パネルを 270° 回転して 640×480 として使用） | Pixel2 版 0.1.4 |
| V90S | Allwinner A133P | Cortex-A53 ×4、最大 1.8GHz | PowerVR Rogue GE8300、OpenGL ES 3.2 | 640×480（約59Hz） | V90S 版 1.0.2（Debian 12 ベース） |

メモリはいずれも約 1GB。glibc はいずれも 2.36。

## 計測方法

- **CPU ガバナー**：計測中は performance（Playload.sh が起動中だけ切り替え、終了時に元へ戻す）。特記があるものを除く。
- **実際のセッション**：plumOS の Ports 起動スクリプト（`plumos-portmaster-port-launch`）経由で、画面・音を出した状態で実行。`PLAYLOAD_STATS=1` が 300 tick（5秒）ごとに出力する値を記録した。
  - **frame avg / max**：プレイヤーがゲームの1 tick を待った時間（Node.js の並行処理が効いていれば 0 に近い）
  - **present**：5秒間に画面を更新した回数（300回 = 60fps）
  - **over 20ms**：表示の間隔が 20ms を超えた回数（見た目の引っかかり）
  - **max（present）**：表示の間隔の最大値
- **自動操作**（`PLAYLOAD_AUTOPLAY=1`）：A ボタンを 60フレームごとに 10フレーム押し、十字キーを上→右→下→左と 45フレームずつ押す。タイトル画面を抜け、ショットを撃ち続ける。rubble. は「どのボタンでも一時停止」する作りなので、自動操作なしで計測した。
- **画面なしの計測**（`--headless`）：1 tick（ゲーム処理＋描画）の時間を連続実行で計測。入力は固定パターン（左右交互、A を 7フレームごと、B を 90フレームごと）。
- 入力なし・固定パターンの計測は、実際のプレイ（弾を撃ち続けるなど）より軽いことがある。

## 1. RK3566（Bubble）

### 1.1 JS エンジンの選定（BRR 3KB、画面なし、9000 tick、ガバナー ondemand・最大クロックに張り付いた状態）

本家の `engine.js` を JS のまま動かした場合の比較。

| 構成 | ゲーム処理 | 描画（JS） | 合計 平均 | 備考 |
|---|---|---|---|---|
| QuickJS | 3.7〜7.7ms | 22〜24ms | 26〜32ms | 60fps に届かない。メモリ 17MB |
| Node.js（V8） | 2〜4ms | 2.5ms | 中央値 4.5〜6.6ms | p99 70〜130ms、最大 360ms の停止。メモリ 150MB |
| Node.js `--jitless` | — | — | QuickJS とほぼ同じ | V8 の速さは JIT によるもの |

音源（AY-3-8910、48kHz を1秒分合成）：QuickJS で CPU 885%、Node.js で 31% → **C に移植**（1 tick 分＝800サンプルで約2.1ms）。

60fps の実時間動作（Node.js）：ondemand で平均 9.99ms・CPU 77%、performance で平均 6.99ms・p99 8.77ms・CPU 61%。

### 1.2 QuickJS ＋ C 描画（BRR 3KB、画面なし、9000 tick、performance）

| 構成 | 0〜60秒 | 60〜120秒 | ボス戦（敵90体） | 最大 |
|---|---|---|---|---|
| エンジンを独自に作り直した版（旧版） | 1.17ms | 1.59ms | 2.45ms | 3.39ms |
| 本家 `engine.js` ＋ `render()` 差し替え（最適化前） | 5.69ms | 7.13ms | 9.50ms | 11.73ms |
| 同上 ＋ `characterKey`・区切り処理のキャッシュ | 3.19ms | 3.72ms | 4.89ms | 6.08ms |

実際のセッション（画面・音あり、ボット操作）：プロセス全体で 1コアの約 40%（音声・描画スレッド込み）、RSS 65MB。

### 1.3 表示（GPU 時間、480×432 に描画）

| 表示 | GPU 時間／フレーム | 結果 |
|---|---|---|
| 通常 | 2.0ms | 60fps |
| CRT（本家シェーダーそのまま：1タップごとに色番号から色を探す） | 21.9ms | 約42fps |
| CRT（sRGB テクスチャで色を渡す、採用版） | 7.0ms | 60fps（本家方式との差は画素で最大 2/255） |

### 1.4 Node.js 版の最適化の経過（AFTER LOAD HD、画面なし）

| 段階 | ゲーム処理 | 描画データ作成 | 合計 平均 |
|---|---|---|---|
| QuickJS（参考） | 約45ms | — | 約52ms |
| Node.js、本家の JS 描画 | 2.6ms | 23ms | 26ms |
| Node.js、C 描画（最適化前） | 2.6ms | 36〜43ms | 40ms |
| ＋ 文字パターンのキャッシュ、`-0` 座標の事前作成（V8 の最適化が外れる問題を解消） | 2.6ms | **2.6ms** | **7.1ms** |

10SEC RACE（`put()` 約250回/フレーム）：区切り処理（ICU）と `characterKey` のキャッシュで 19.6ms → 7.7ms。

### 1.5 Node.js 版、全ゲーム（画面なし、900 tick、performance、最適化後）

| ゲーム | 平均 | p95 | 最大 |
|---|---|---|---|
| 10SEC RACE | 7.70ms | 8.68ms | 25.65ms |
| AFTER LOAD HD | 7.53ms | 10.68ms | 48.21ms |
| Block crash | 1.42ms | 2.10ms | 16.49ms |
| Dungeon Escape | 3.31ms | 4.29ms | 57.67ms |
| Puchi Jump | 4.96ms | 5.85ms | 21.43ms |
| BRR 3KB | 1.36ms | 2.00ms | 16.69ms |
| chiptune | 1.31ms | 2.56ms | 16.13ms |
| glyph-quest | 1.17ms | 1.81ms | 17.11ms |
| jump | 1.11ms | 1.68ms | 21.53ms |
| rubble. | 12.54ms | 14.29ms | 34.66ms |
| shooter | 1.26ms | 1.88ms | 16.44ms |
| snake | 1.49ms | 4.00ms | 15.74ms |

### 1.6 並行処理（パイプライン）の効果（rubble.、実際のセッション、CRT ON、入力なし）

| | present（5秒あたり） | over 20ms | frame avg |
|---|---|---|---|
| 並行処理なし | 93〜191回（約19〜38fps） | 93〜136回 | 10.7〜13.0ms |
| 並行処理あり（採用） | 300回（60fps） | 0〜2回 | 0.3〜0.6ms（起動直後の5秒を除く） |

ユーザーの目視確認：変更前は FPS 19 / TPS 60 / MS 11.8 / MAX 15.7、AFTER LOAD HD はほぼ 60fps。

### 1.7 全ゲームの実際のセッション（20秒、CRT ON、Node.js）

**起動から5秒以降**：全ゲームで present 300回、over 20ms 0回、表示間隔の最大 16.8〜17.4ms。例外は自動操作時の Dungeon Escape と chiptune で各1回だけ 20.0ms。

**起動直後の5秒**（V8 の JIT コンパイルが集中する時間帯）：

| ゲーム | 入力なし：over 20ms／最大間隔 | 自動操作：over 20ms／最大間隔 | 自動操作：安定後の frame avg |
|---|---|---|---|
| 10SEC RACE | 7回／55.0ms | 6回／75.1ms | 0.09ms |
| AFTER LOAD HD | 7回／84.8ms | 5回／70.4ms | 0.98ms |
| Block crash | 7回／57.2ms | 6回／61.1ms | 0.10ms |
| Dungeon Escape | 7回／60.8ms | 5回／90.1ms | 0.08ms |
| Puchi Jump | 6回／57.7ms | 6回／59.1ms | 0.08ms |
| BRR 3KB | 12回／54.7ms | 7回／61.5ms | 0.11ms |
| chiptune | 6回／67.1ms | 7回／55.9ms | 0.09ms |
| glyph-quest | 24回／64.4ms | 8回／56.7ms | 0.08ms |
| jump | 7回／55.2ms | 6回／53.7ms | 0.08ms |
| rubble. | 21回／85.3ms | （自動操作なしで計測） | — |
| shooter | 7回／59.1ms | 6回／63.9ms | 0.08ms |
| snake | 10回／55.7ms | 6回／65.4ms | 0.08ms |

## 2. RK3326（GKD Pixel2）

実際のセッション、Node.js、CRT OFF、20秒（rubble. 以外は自動操作）。

| ゲーム | present（5秒ごと） | over 20ms | 最大間隔 | frame avg |
|---|---|---|---|---|
| AFTER LOAD HD | 226 / 241 / 269回（約45〜54fps） | 66 / 43 / 28回 | 83.6 / 78.1 / 71.3ms | 6.9〜8.7ms |
| rubble. | 153 / 149回（約30fps） | 153 / 149回 | 160.9 / 131.4ms | 23.0〜24.4ms |
| 10SEC RACE | 257 / 300 / 301回 | 41 / 0 / 0回 | 97.1 / 19.8 / 18.5ms | 1.0〜7.4ms |
| Puchi Jump | 296 / 301 / 300回 | 7 / 0 / 0回 | 38.1 / 17.2 / 16.8ms | 0.2〜0.6ms |
| BRR 3KB | 300 / 300 / 301回 | 3 / 0 / 0回 | 32.2 / 16.9 / 17.0ms | 0.2〜0.4ms |

- CRT ON：ユーザーの目視確認で、全ゲームが 60fps を下回った（数値は未記録）。→ **CRT OFF 推奨**
- 重量級の2本は、Node.js の1フレームの計算が 16.7ms を超えるため、表示側の工夫では 60fps に届かない。

## 3. A133P（V90S）

実際のセッション、Node.js、20秒（rubble. 以外は自動操作）。この機体の画面は約59Hz のため、present の上限は約295回。

| ゲーム | CRT OFF：present（5秒ごと） | CRT OFF：over 20ms／最大間隔 | CRT ON：present | CRT ON：frame avg |
|---|---|---|---|---|
| AFTER LOAD HD | 293 / 295 / 295回 | 5・0・0回 ／ 38.2・17.2・18.2ms | 147〜148回（30fps） | 5.3〜5.9ms |
| rubble. | 243 / 295 / 295回 | 37・2・0回 ／ 82.5・22.4・18.3ms | 142〜148回（30fps） | 8.1〜12.3ms |
| 10SEC RACE | 296 / 295 / 295回 | 1・0・0回 ／ 25.6・17.1・17.2ms | 147〜149回（30fps） | 4.4〜4.8ms |
| Puchi Jump | 296 / 295 / 295回 | 0・0・0回 ／ 19.1・17.3・17.2ms | 147〜149回（30fps） | 2.7〜2.9ms |
| BRR 3KB | 296 / 295 / 295回 | 0・0・0回 ／ 17.1・17.1・17.1ms | 148〜149回（30fps） | 1.1〜1.3ms |

- CRT OFF：重量級の2本を含め、全ゲームが約59fps（rubble. は最初の5秒だけ 243回）。
- CRT ON：GPU（PowerVR GE8300）で CRT 処理が 16.7ms に収まらず、ゲームの重さに関係なく 30fps。→ **CRT OFF 推奨**

## まとめ

| SoC | 推奨設定 | 普通のゲーム | 重量級（AFTER LOAD HD / rubble.、CRT OFF） |
|---|---|---|---|
| RK3566 | CRT ON でも可 | 60fps | 60fps / 60fps |
| RK3326 | CRT OFF | 60fps | 約45〜54fps / 約30fps |
| A133P | CRT OFF | 約59fps（画面の上限） | 約59fps / 約59fps |

全機種で共通して、Node.js 版は**起動直後の数秒だけ**、V8 の JIT コンパイルによる短い引っかかり（最大 50〜160ms）がある。

## 再計測の手順（参考）

- 画面なし：`./playload.aarch64 --headless --runtime node --ticks 900 games/xxx.url`（`PLAYLOAD_NO_INPUT=1` で入力なし、`PLAYLOAD_PROFILE=1` で Node.js 側の内訳）
- 実際のセッション：`PLAYLOAD_STATS=1 PLAYLOAD_AUTOPLAY=1 PLAYLOAD_RUNTIME=node sh <plumOS>/bin/plumos-portmaster-port-launch <Ports>/Playload.sh games/xxx.url` を実行し、`playload/log.txt` の `frame avg` 行を読む
- GPU 時間：`PLAYLOAD_GPU_TIMING=1`（表示ごとに `glFinish` で待つため、診断専用）
- V90S 版 plumOS は、SSH を閉じると裏のプロセスが止まり、`/tmp` も接続ごとに別になる。計測は接続を開いたまま、結果は `/mnt/plumos/state/` に保存する。
