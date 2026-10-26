param(
    [Parameter(Position = 0)]
    [ValidateSet('build', 'rebuild', 'clean')]
    [string]$Action,

    [Parameter(Position = 1)]
    [ValidateSet('debug', 'release')]
    [string]$Configuration,

    [Parameter(Position = 2)]
    [ValidateSet('x64', 'x86')]
    [string]$Platform
)

<# ========== 输出格式定义 ========== #>

# 1. 节点执行信息：蓝色，用于操作节点开始、动作提示
function Write-NodeInfo {
    param([string]$Message)
    Write-Host "[信息]：$Message" -ForegroundColor Blue
}

# 2. 内容信息：深灰色，用于文件内容、数据详情输出
function Write-ContentInfo {
    param([string]$Message)
    Write-Host "[信息]：$Message" -ForegroundColor DarkGray
}

# 3. 错误信息：红色，用于异常、终止执行提示
function Write-ErrorInfo {
    param([string]$Message)
    Write-Host "[错误]：$Message" -ForegroundColor Red
}

# 4. 成功信息：绿色，用于操作完成、执行成功提示
function Write-SuccessInfo {
    param([string]$Message)
    Write-Host "[信息]：$Message" -ForegroundColor Green
}

<# ========== 输出 build_config.json 模板 ========== #>
function Write-ConfigTemplate {
    $template = 
@"
{
    "MSBuild_path": "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe",
    "solution_file": "C:\\Path\\To\\YourSolution.sln"
}
"@
    Write-ContentInfo "======================== build_config.json 模板 ========================"
    Write-ContentInfo ""
    Write-ContentInfo $template
    Write-ContentInfo ""
    Write-ContentInfo "请将以上内容复制到当前目录下的 build_config.json 文件中，"
    Write-ContentInfo "并根据实际情况修改 MSBuild_path 和 solution_file 的值，"
    Write-ContentInfo "然后重新运行本脚本。"
    Write-ContentInfo "========================================================================"
}

<# ========== 配置文件读取与校验 ========== #>
Write-NodeInfo "开始读取构建配置文件 build_config.json"

# 配置文件路径（当前工作目录）
$configFileName = "build_config.json"
$configPath = Join-Path -Path (Get-Location).Path -ChildPath $configFileName

# 1. 检查配置文件是否存在
if (-not (Test-Path -Path $configPath -PathType Leaf)) {
    Write-ErrorInfo "当前目录下未找到配置文件 $configFileName，脚本终止执行"
    Write-ConfigTemplate
    exit 1
}

# 2. 读取并解析 JSON，捕获格式错误
try {
    $configContent = Get-Content -Path $configPath -Raw -Encoding UTF8
    $config = $configContent | ConvertFrom-Json
}
catch {
    Write-ErrorInfo "配置文件 JSON 格式解析失败：$($_.Exception.Message)，脚本终止执行"
    Write-ConfigTemplate
    exit 1
}

# 3. 校验必填字段：MSBuild_path
if (-not ($config.PSObject.Properties.Name -contains 'MSBuild_path') -or [string]::IsNullOrWhiteSpace($config.MSBuild_path)) {
    Write-ErrorInfo "配置文件缺少必填字段 MSBuild_path 或值为空，脚本终止执行"
    Write-ConfigTemplate
    exit 1
}
if (-not (Test-Path -Path $config.MSBuild_path -PathType Leaf)) {
    Write-ErrorInfo "MSBuild 可执行文件路径不存在：$($config.MSBuild_path)，脚本终止执行"
    Write-ConfigTemplate
    exit 1
}

# 4. 校验必填字段：solution_file
if (-not ($config.PSObject.Properties.Name -contains 'solution_file') -or [string]::IsNullOrWhiteSpace($config.solution_file)) {
    Write-ErrorInfo "配置文件缺少必填字段 solution_file 或值为空，脚本终止执行"
    Write-ConfigTemplate
    exit 1
}
if (-not (Test-Path -Path $config.solution_file -PathType Leaf)) {
    Write-ErrorInfo "解决方案文件不存在：$($config.solution_file)，脚本终止执行"
    Write-ConfigTemplate
    exit 1
}

# 5. 输出指定配置内容
Write-ContentInfo "MSBuild_path          : $($config.MSBuild_path)"
Write-ContentInfo "solution_file         : $($config.solution_file)"

Write-SuccessInfo "配置文件读取与校验通过"

<# ========== 编译参数校验与处理 ========== #>
Write-NodeInfo "校验编译输入参数"

# 校验必传操作参数
if ([string]::IsNullOrWhiteSpace($Action)) {
    Write-ErrorInfo "缺少必传参数：操作类型（支持 build/rebuild/clean），脚本终止执行"
    exit 1
}

# 处理可选参数默认值
if ([string]::IsNullOrWhiteSpace($Configuration)) {
    $Configuration = 'Debug'
}
if ([string]::IsNullOrWhiteSpace($Platform)) {
    $Platform = 'x64'
}

# 格式化名称（首字母大写，匹配 MSBuild/VS 命名规范）
$targetName = [char]::ToUpper($Action[0]) + $Action.Substring(1).ToLower()
$configName = [char]::ToUpper($Configuration[0]) + $Configuration.Substring(1).ToLower()

# 输出最终编译参数
Write-ContentInfo "编译目标 Target      : $targetName"
Write-ContentInfo "编译配置 Configuration : $configName"
Write-ContentInfo "目标平台 Platform    : $Platform"

<# ========== 执行 MSBuild 编译 ========== #>
Write-NodeInfo "开始执行 MSBuild 编译"

# 组装参数数组，自动处理路径含空格的解析问题
$msbuildArgs = @(
    $config.solution_file,
    "/t:$targetName",
    "/p:Configuration=$configName;Platform=$Platform",
    "/v:minimal",
    "/fl",
    "/flp:logfile=build.log;errorsonly"
)

& $config.MSBuild_path $msbuildArgs
