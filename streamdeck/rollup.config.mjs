import commonjs from "@rollup/plugin-commonjs";
import nodeResolve from "@rollup/plugin-node-resolve";
import terser from "@rollup/plugin-terser";
import typescript from "@rollup/plugin-typescript";

// Structure and plugin order follow @elgato/cli 1.9.0's installed template.
export default {
  input: "src/plugin.ts",
  output: {
    file: "com.gomtwigim.livemix.sdPlugin/bin/plugin.js",
    format: "es",
    sourcemap: !!process.env.ROLLUP_WATCH
  },
  plugins: [
    typescript(),
    nodeResolve({ browser: false, exportConditions: ["node"], preferBuiltins: true }),
    commonjs(),
    !process.env.ROLLUP_WATCH && terser(),
    {
      name: "emit-module-package-file",
      generateBundle() {
        this.emitFile({ fileName: "package.json", source: '{ "type": "module" }\n', type: "asset" });
      }
    }
  ]
};
