# 独立リポジトリ化メモ（ゆらてぃー向け）

Cloud Agent 環境では `yuratexi/m_ui_preset_aviutl2` の新規作成権限が無く、
本プラグインを一時的に `m_toolbar_aviutl2` リポジトリ内へ自己完結フォルダとして追加しています。

## 推奨次ステップ

1. GitHub で `yuratexi/m_ui_preset_aviutl2` を新規作成（public / MIT）
2. このディレクトリ `m_ui_preset_aviutl2/` の内容を新リポジトリのルートへ移す
3. 本 PR をマージするか、移設完了後に `m_toolbar_aviutl2` 側からフォルダを削除する

ビルドは移設後も同じです: `build.bat Release`
