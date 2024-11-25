import pandas as pd
import numpy as np
import plotly.graph_objects as go

# 데이터 준비 (실제 데이터로 대체)
data = {
    (f"192.168.0.{i}", f"10.0.0.{j}"): (i * j % 100, i + j)
    for i in range(1, 1001)
    for j in range(1, 1001)
}

# 데이터를 리스트로 변환하여 DataFrame 생성
records = [
    {"source": src, "destination": dst, "value": val, "time": time}
    for (src, dst), (val, time) in data.items()
]
df = pd.DataFrame(records)

# IP 주소를 인덱스로 매핑
source_ips = df["source"].unique()
destination_ips = df["destination"].unique()
source_ip_to_index = {ip: idx for idx, ip in enumerate(source_ips)}
destination_ip_to_index = {ip: idx for idx, ip in enumerate(destination_ips)}
df["source_idx"] = df["source"].map(source_ip_to_index)
df["destination_idx"] = df["destination"].map(destination_ip_to_index)

# 피벗 테이블 생성 (value)
pivot_table = df.pivot(
    index="destination_idx", columns="source_idx", values="value"
).fillna(0)
z_values = pivot_table.values

# 인덱스를 IP 주소로 매핑
index_to_source_ip = {idx: ip for ip, idx in source_ip_to_index.items()}
index_to_destination_ip = {idx: ip for ip, idx in destination_ip_to_index.items()}

# 호버 데이터 생성
x_indices = pivot_table.columns.values
y_indices = pivot_table.index.values
x_ips = [index_to_source_ip[idx] for idx in x_indices]
y_ips = [index_to_destination_ip[idx] for idx in y_indices]
x_mesh, y_mesh = np.meshgrid(x_ips, y_ips)

# 'time' 데이터를 피벗 테이블 형태로 변환
time_pivot = df.pivot(index="destination_idx", columns="source_idx", values="time")
time_values = time_pivot.values
time_values = np.where(pd.isnull(time_values), "N/A", time_values)

# customdata 생성
customdata = np.dstack((x_mesh, y_mesh, time_values))

# 히트맵 생성
fig = go.Figure(
    data=go.Heatmap(
        z=z_values,
        colorscale="Viridis",
        colorbar=dict(title="Value"),
        hoverongaps=False,
        customdata=customdata,
        x=x_indices,
        y=y_indices,
    )
)

# 호버 템플릿 설정
fig.update_traces(
    hovertemplate="Source IP: %{customdata[0]}<br>"
    + "Destination IP: %{customdata[1]}<br>"
    + "Value: %{z}<br>"
    + "Time: %{customdata[2]}<extra></extra>"
)

# 레이아웃 업데이트
fig.update_layout(
    xaxis_title="Source IP",
    yaxis_title="Destination IP",
    title="Source-Destination Heatmap",
)

# 축 라벨 숨기기
fig.update_xaxes(visible=False)
fig.update_yaxes(visible=False)

# HTML 파일로 저장
fig.write_html("heatmap_with_time.html")
