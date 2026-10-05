# Builds libnicklegpt.so and KoboRoot.tgz with the NickelTC Docker image.
# Usage: .\build.ps1            (build + KoboRoot.tgz)
#        .\build.ps1 clean
param([string]$Target = "")

$image = "ghcr.io/pgaskin/nickeltc:1.0"
$src = $PSScriptRoot

if ($Target -eq "clean") {
    docker run --rm -v "${src}:/src" -w /src $image make clean
    exit $LASTEXITCODE
}

# Version label from git (e.g. 0.1.0, or 0.1.0-3-gabc1234-dirty between tags).
$version = git -C $src describe --tags --always --dirty 2>$null
if (-not $version) { $version = "dev" }
$version = $version -replace '^v', ''

docker run --rm -v "${src}:/src" -w /src $image sh -c "make NGPT_VERSION=$version && make koboroot NGPT_VERSION=$version"
exit $LASTEXITCODE
