"""생성된 시퀀스의 카메라 모션 크기가 레퍼런스에서 잰 값과 같은지 본다.

다른 구도 스위트는 전부 **레퍼런스 영상 분석**을 잰다. 만들어 낸 결과를 되재는 것은
`retarget`과 이 스위트뿐이다. 이게 없는 동안 "모션 종류는 맞는데 크기는 상수"인 상태가
오래 드러나지 않았다.

재는 것은 두 가지다.
- 생성된 시작·끝 프레임에서 배경 특징점으로 되잰 값(픽셀). 배경에 무늬가 없는 레벨에서는 못 잰다.
- 생성된 카메라로 정적 월드 점을 다시 투영해 얻은 값(기하). 레벨과 무관하게 항상 잴 수 있다.

이 스위트가 보장하는 것은 **플랜 → 솔버 → 시퀀스 빌더 사슬의 자기 일관성**이다.
추정값이 실제 카메라와 얼마나 맞는지(정확도)는 모션 정답 영상과 비교한 쪽에서 재며,
그 수치는 Docs/CAMERA_MOTION.md에 있다. 둘은 다른 주장이고 둘 다 필요하다.

정답 데이터: UPT.ReferenceE2E가 Saved/UniversalProductionTools/GroundTruth/motion_roundtrip_<시각>/ 에 남긴다.
"""
from __future__ import annotations

import json
import os

import numpy as np

from common import latest_dataset, use_utf8_output


def run(output_dir: str) -> dict:
    data_dir = latest_dataset("motion_roundtrip")
    if not data_dir:
        return {"skipped": "정답 데이터 없음(motion_roundtrip). UPT.ReferenceE2E를 모션이 있는 레퍼런스 플랜으로 돌리세요"}
    path = os.path.join(data_dir, "verify_report.json")
    if not os.path.isfile(path):
        return {"skipped": f"verify_report.json 없음({os.path.basename(data_dir)})"}
    with open(path, encoding="utf-8") as handle:
        report = json.load(handle)

    rows = [row for row in report.get("shots", []) if row.get("motion_geometry")]
    # 핸드헬드는 폭만 재현하고 방향은 상수라 위치 오차를 재는 것이 의미가 없다. 따로 센다.
    directional = [r for r in rows if r.get("camera_motion") != "handheld"]
    dx = [r["motion_dx_error"] for r in rows if r.get("motion_dx_error") is not None
          and r.get("camera_motion") != "handheld"]
    scale = [abs(np.log(r["motion_scale_ratio"])) for r in rows
             if r.get("motion_scale_ratio") not in (None, 0)]

    os.makedirs(output_dir, exist_ok=True)
    metrics = {
        "motion_shots": len(rows),
        "shots_with_geometry": len(rows),
        "dx_error_median": round(float(np.median(dx)), 4) if dx else None,
        "dx_error_worst": round(float(max(dx)), 4) if dx else None,
        "scale_log_error_worst": round(float(max(scale)), 4) if scale else None,
        "unmeasured": sum(1 for r in directional
                          if r.get("motion_dx_error") is None and r.get("motion_scale_ratio") is None),
        "dataset": os.path.basename(data_dir),
    }
    with open(os.path.join(output_dir, "motion_roundtrip_rows.json"), "w", encoding="utf-8") as handle:
        json.dump({"metrics": metrics, "rows": rows}, handle, ensure_ascii=False)
    return metrics


if __name__ == "__main__":
    use_utf8_output()
    print(json.dumps(run(os.path.join(os.getcwd(), "eval_output")), ensure_ascii=False, indent=2))
