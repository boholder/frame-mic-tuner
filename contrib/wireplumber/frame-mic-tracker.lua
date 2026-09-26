-- SPDX-License-Identifier: MIT
-- Steam Frame のマイクのフィルターを、設定に合わせてオン・オフする WirePlumber スクリプト。
-- Valve の /etc/wireplumber/scripts/microphone-tracker.lua の代わりに動く（元のファイルは書き換えない）。
--
-- 元のスクリプトと同じく、マイクを使うアプリがいる間だけフィルター（EQ・エコー除去・ノイズ除去）を有効にする。
-- 違いは、エコー除去とノイズ除去をそれぞれ WirePlumber の設定で切れること。
--   wpctl settings --save frame-mic.echo-cancel false       # エコー除去を切る（すぐ反映・再起動後も残る）
--   wpctl settings --save frame-mic.noise-suppression true  # ノイズ除去を入れる
-- 設定を変えると、使用中のマイクにもその場で反映する（PipeWire の再起動は要らない）。

local state = {
  stream_count = 0,
}

-- フィルターのノード（Valve の設定で steamos.mic_filter = true が付いているもの）
local filters_om = ObjectManager {
  Interest {
    type = "node",
    -- ノードの情報側のプロパティなので type = "pw" が要る（既定の pw-global だと一致しない）
    Constraint { "steamos.mic_filter", "=", "true", type = "pw" },
  }
}

-- フィルターの有効・無効を書き込む先（filters メタデータ）
local metadata_om = ObjectManager {
  Interest {
    type = "metadata",
    Constraint { "metadata.name", "=", "filters" },
  }
}

-- ノード名から、どの設定でオン・オフするかを決める
-- 戻り値: 設定のキー。nil なら常にマイクの使用状況だけで決める（EQ など）
local function setting_for (node)
  local name = node.properties ["node.name"] or ""
  if name:find ("^echo_cancel") then
    return "frame-mic.echo-cancel"
  end
  if name:find ("^ns_") or name:find ("^dsp_") then
    return "frame-mic.noise-suppression"
  end
  return nil
end

-- 1 つのフィルターを今の状態に合わせる
local function apply_node (metadata, node)
  local disabled = state.stream_count == 0
  local key = setting_for (node)
  if not disabled and key ~= nil and not Settings.get_boolean (key) then
    disabled = true
  end
  local node_id = node.properties ["object.id"]
  metadata:set (node_id, "filter.smart.disabled", "Spa:String:JSON",
      disabled and "true" or "false")
end

-- すべてのフィルターを今の状態に合わせる
local function apply_all ()
  local metadata = metadata_om:lookup ()
  if metadata == nil then
    return
  end
  for node in filters_om:iterate () do
    apply_node (metadata, node)
  end
end

filters_om:connect ("object-added", function (_, node)
  local metadata = metadata_om:lookup ()
  if metadata ~= nil then
    apply_node (metadata, node)
  end
end)
metadata_om:connect ("object-added", function ()
  apply_all ()
end)

filters_om:activate ()
metadata_om:activate ()

Settings.subscribe ("frame-mic.*", function ()
  apply_all ()
end)

SimpleEventHook {
  name = "frame-mic-stream-added",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "node-added" },
      Constraint { "media.class", "=", "Stream/Input/Audio" },
      Constraint { "node.virtual", "!", "true" },
    }
  },
  execute = function (event)
    state.stream_count = state.stream_count + 1
    if state.stream_count == 1 then
      apply_all ()
    end
  end
}:register ()

SimpleEventHook {
  name = "frame-mic-stream-removed",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "node-removed" },
      Constraint { "media.class", "=", "Stream/Input/Audio" },
      Constraint { "node.virtual", "!", "true" },
    }
  },
  execute = function (event)
    state.stream_count = math.max (0, state.stream_count - 1)
    if state.stream_count == 0 then
      apply_all ()
    end
  end
}:register ()
