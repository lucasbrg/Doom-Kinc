let project = new Project('Doom-Kinc');

project.addFile('src/**');
project.addIncludeDir('src');
project.setDebugDir('Deployment');

resolve(project);
