module.exports = {
  // 'main' is the stable release channel. It only ever advances by fast-
  // forwarding to dev's tip during a minor/major promotion (release-
  // semantic.yaml) — hotfixes never touch it directly, so it can't diverge
  // from dev between promotions.
  //
  // 'dev' is the integration channel and produces vX.Y.Z-rc.N prereleases.
  //
  // 'hotfix/X.Y.x' is a release branch for ANY line, current or older,
  // treated uniformly. It has no `range`: release-semantic.yaml prunes every
  // tag not reachable from the dispatched hotfix branch before invoking
  // semantic-release, which scopes its version-ordering view to that
  // branch's own lineage instead of the shared array order `range` would
  // otherwise require.
  branches: [
    'main',
    { name: 'dev', prerelease: 'rc' },
    {
      name: 'hotfix/+([0-9])?(.{+([0-9]),x}).x',
      channel: '${name.split("/")[1]}',
    },
  ],
  plugins: [
    '@semantic-release/commit-analyzer',
    '@semantic-release/release-notes-generator',
    [
      '@google/semantic-release-replace-plugin',
      {
        replacements: [
          {
            files: ['CMakeLists.txt'],
            from: 'VERSION [0-9]+\\.[0-9]+\\.[0-9]+',
            // Strip prerelease suffix so CMake gets '1.5.0' not '1.5.0-rc.1'.
            // No results assertion: stable after RC is a no-op (version already set).
            to: "VERSION ${nextRelease.version.split('-')[0]}",
          },
        ],
      },
    ],
    [
      '@semantic-release/git',
      {
        assets: ['CMakeLists.txt', 'features/**/Shaders/Features/*.ini'],
        message: 'chore(release): ${nextRelease.version} [skip ci]',
      },
    ],
    [
      '@semantic-release/github',
      {
        draftRelease: true,
        assets: [],
        successComment: false,
        failComment: false,
        releasedLabels: false,
      },
    ],
  ],
};
