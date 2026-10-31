# Playload Handheld

**[Playload](https://github.com/kitao/playload) のゲームを、Linux 携帯ゲーム機でオフラインで遊ぶ。**

[Playload](https://kitao.dev/playload/) は、Takashi Kitao さんによる小さなゲームエンジンで、ゲームがまるごと URL の中に入っています。Playload Handheld は plumOS 携帯ゲーム機向けの PortMaster 形式の Port です。ゲームの URL を `.url` ファイルに保存して機体にコピーすれば、機体のボタンで遊べます。

[English](README.md)

> Playload Handheld は非公式のプロジェクトです。Playload プロジェクトおよびその作者とは関係がなく、承認を受けたものでもありません。

| ゲームメニュー | CRT 表示 |
|---|---|
| ![ゲームメニュー](docs/images/menu.png) | ![CRT 表示の Bit-Rate Rush 3KB](docs/images/brr3kb-crt.png) |
| ![Glyph Quest](docs/images/glyph-quest.png) | ![CRT 表示の Shooter](docs/images/shooter-crt.png) |

## 特長

- Playload のゲーム URL を、RK3566 クラスの機体なら非常に重いゲームも含めて 60fps で実行
- 本家の Playload エンジンをそのまま使うので、ブラウザ版と同じ見た目・音（画面と音の一致を検証済み）
- 本家の CRT シェーダーによる CRT 表示（切り替え可）
- 前回遊んだゲームを覚えているゲームメニュー
- 本家の作品集 [Playload Arcade](https://kitao.dev/playload/arcade/) を機体で閲覧。各ゲームをその場で動かしてプレビューし、そのまま保存（Wi-Fi）
- Nintendo 配置・Xbox 配置のボタンに対応
- アプリ内から Playload エンジンを更新（Wi-Fi）。切り替える前に自己テストを実行
- 性能表示（FPS、1秒あたりの処理回数、処理時間）

## 対応機種

plumOS で動作を確認しています。

| 機種 | SoC | 推奨設定 | 重いゲーム* |
|---|---|---|---|
| plumOS-Bubble | Rockchip RK3566 | CRT ON／OFF どちらでも可 | 60fps |
| GameKiddy GKD Pixel2 | Rockchip RK3326 | CRT **OFF** | 30〜54fps |
| V90S | Allwinner A133P | CRT **OFF** | 約59fps（画面の上限） |

\* 確認できた中で特に重い Playload ゲーム（AFTER LOAD HD と rubble.）での値です。普通のゲームは3機種とも全速で動きます。詳しくは [docs/BENCHMARKS.ja.md](docs/BENCHMARKS.ja.md) を参照してください。

PortMaster が動く、ほかの 64bit ARM Linux 携帯機でも動く可能性がありますが、未確認です。

## 導入方法

1. [Releases](../../releases) から `playload-handheld-<バージョン>.zip` をダウンロードします。
2. 機体の Ports フォルダに展開します。`Playload.sh` と `playload` フォルダが並ぶように置いてください。

   | plumOS の種類 | Ports フォルダ |
   |---|---|
   | Bubble | SD カードの `Roms/PORTS/`（`/storage/user/Roms/PORTS/`） |
   | Pixel2 | ROMS カードの `PORTS/`（`/roms/PORTS/`） |
   | V90S | SD カードの `roms/ports/`（`/mnt/plumos/roms/ports/`） |

3. フロントエンドの PORTS から **Playload** を起動します。

更新するときは、新しい zip を上から展開してください。設定、ゲーム、ダウンロード済みのエンジンはそのまま残ります。

## ゲームの追加

### Playload Arcade から（機体だけで）

メニューの `ARCADE` を選んで A を押します（Wi-Fi が必要です。最後に取得した一覧は、オフラインでも使えます）。

- 作品集のゲームが新しい順に並びます。`*` は保存済みのゲームです。
- A でゲームの詳細を開き、タイトル画面をその場で動かしてプレビューします。左右で前後のゲームに移ります。
- もう一度 A を押すと `playload/games/` に保存します。保存済みなら A で遊べて、終わると同じゲームの画面に戻ります。B で戻ります。

フォントで描けないタイトルや作者名（ほとんどの漢字）は、スクリーンショットの名前や X のアカウント名で表示します。とても重いゲームは、プレビューではゆっくり動きます。

### 手作業で

Playload のゲームは、URL そのものがゲームです。追加の手順は次のとおりです。

1. ブラウザでゲームを開き、アドレス欄の**完全な URL** をコピーします。`https://kitao.dev/playload/app/#v1.` で始まる長いものです。
   - X のポストのリンクは短縮 URL（`https://t.co/…`）になっていて、ゲームが入っていません。一度リンクを開いてから、ブラウザに表示された URL をコピーしてください。
2. テキストファイルに貼り付けて、`好きな名前.url` で保存します（1行だけで構いません）。
3. 機体の `playload/games/` にコピーします。

メニューには、ファイル名ではなくゲーム自身のタイトル（`// @title` の行）が表示されます。Playload のゲームの JavaScript ファイル（`.js`）もそのまま使えます。

## 操作方法

| ボタン | ゲーム中 | メニュー |
|---|---|---|
| 十字キー／左スティック | Playload の方向キー | 移動（長押しで連続移動）。ゲーム一覧では左右でページ送り |
| A | Playload の A | ゲームを始める／設定を変える／Arcade を開く・ゲームを見る・保存する |
| B | Playload の B | 戻る（Arcade） |
| SELECT ＋ START | メニューに戻る | 終了 |
| SELECT ＋ R1 | CRT 表示の ON／OFF | CRT 表示の ON／OFF |
| SELECT ＋ L1 | 性能表示の ON／OFF | 性能表示の ON／OFF |

メニューは、前回遊んだゲームにカーソルがある状態で開きます。

## 設定

メニューの下の行が設定です。上下で選び、A か左右で変更します。設定は `playload/playload.cfg` に保存されます。

| 設定 | 値 | |
|---|---|---|
| `ARCADE` | — | **A**：Playload Arcade を開く（[ゲームの追加](#ゲームの追加)を参照） |
| `BUTTONS` | `NINTENDO` / `XBOX` | A と B を入れ替えます。機体のボタンの印字に合う方を選んでください |
| `CRT` | `ON` / `OFF` | 本家の CRT 表示。RK3326 と A133P では OFF を推奨 |
| `ENGINE` | `builtin`／ダウンロード済みの版 | **A**：GitHub で新しい Playload のリリースを確認し、テストしてから切り替え。**左右**：インストール済みの版を切り替え |
| `RUNTIME` | `NODE.JS` / `QUICKJS` | ゲームを動かす JavaScript エンジン。Node.js の方がはるかに速い。Node.js が起動できない場合は自動で QuickJS を使います |

### 性能表示

SELECT ＋ L1 で、左上に2行の表示が出ます。

![性能表示](docs/images/fps-overlay.png)

- `60FPS`：1秒間に画面を更新した回数
- `60TPS`：1秒間にゲームの処理を実行した回数。60 未満だとゲーム自体が遅くなっています
- `0.1MS MAX 1.7`：直近1秒間に、プレイヤーがゲームの処理を待った時間の平均と最大

Node.js はゲームの開始から数秒かけてゲームを最適化するので、開始直後に一瞬引っかかるのは正常な動きです。

## ファイル構成

```
Playload.sh                 起動スクリプト（Ports フォルダ）
playload/
├── playload.aarch64        プレイヤー本体
├── games/                  .url ファイル
├── playload.cfg            設定（最初に変更したときに作られる）
├── engine/<版>/            アプリ内更新でダウンロードした Playload エンジン
├── arcade/arcade.json      最後に取得した Playload Arcade の一覧
├── node/                   Node.js、そのライブラリ、組み込みのエンジン
├── licenses/               ライセンス文書と第三者の著作物の一覧
└── log.txt                 直近の実行ログ（トラブル時の確認用）
```

## 困ったときは

- **ゲームで ERROR と表示される**：画面のメッセージと `playload/log.txt` に理由が出ます。新しい Playload 向けに作られたゲームは、新しいエンジンが必要な場合があります。`ENGINE` を選んで A を押してください。
- **エンジンの更新や Arcade で NO CONNECTION と出る**：先に機体を Wi-Fi につないでください。
- **動きが遅い**：CRT 表示を OFF にし（SELECT ＋ R1）、`RUNTIME` が `NODE.JS` になっているか確認してください。
- **A と B が逆**：メニューの `BUTTONS` を切り替えてください。

## 開発者の方へ

アーキテクチャ、仕様、性能チューニングの内容、ビルドと配布物の作り方は [docs/DEVELOPMENT.ja.md](docs/DEVELOPMENT.ja.md) にまとめています。

## ライセンス

Playload Handheld は [MIT ライセンス](LICENSE)で公開しています。Playload（MIT）と QuickJS-ng（MIT）を含み、配布物には Node.js（MIT）と Debian の GNU ライブラリ（LGPL／GCC ランタイムライブラリ例外付き GPL）を同梱しています。詳しくは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。

Playload を作られた Takashi Kitao さんに感謝します。
